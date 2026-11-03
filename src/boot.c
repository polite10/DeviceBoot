/* DeviceBoot: one application slot; UART stop-and-wait updates. */
#include "stm32u5xx_hal.h"
#include "device_contract.h"
#include "am_port.h"
#include "am_nor_stm32.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

__attribute__((section(".boot_info"),used))
const device_boot_info_t boot_info={DEVICE_BOOT_MAGIC,4,DEVICE_APP_BASE,DEVICE_APP_MAX,1,0x481,{921600,DEVICE_APP_ID_MAX}};
static UART_HandleTypeDef uart;
static uint32_t image_size,image_crc,image_id,written;
static bool updating;
static uint8_t payload[1040] __attribute__((aligned(16)));
static uint8_t last_response[280];
static uint32_t last_seq,last_request_crc,last_reply_size;
static bool have_reply;
static uint32_t crc_table[256];
static bool nor_ready,storing;
/* Counting down to the application; the last packet's command. */
static bool launching;
static uint32_t handled;
static uint32_t store_slot,store_written;
static am_header_t store_header;

uint32_t device_crc_update(uint32_t crc,const uint8_t *p,uint32_t n) {
    while(n--) crc=crc_table[(crc^*p++)&255]^(crc>>8);
    return crc;
}
static uint32_t checksum(const void *p,uint32_t n) { return ~device_crc_update(0xFFFFFFFFU,p,n); }
static void crc_init(void) {
    for(uint32_t i=0;i<256;++i){ uint32_t c=i;for(unsigned b=0;b<8;++b)c=(c>>1)^(0xEDB88320U&(0U-(c&1)));crc_table[i]=c; }
}
void SysTick_Handler(void) { HAL_IncTick(); }
void Error_Handler(void) { while(1){} }
void assert_failed(uint8_t *f,uint32_t l) { (void)f;(void)l;Error_Handler(); }
static void clock_init(void) {
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);
    HAL_PWREx_ConfigSupply(PWR_SMPS_SUPPLY);
    RCC_OscInitTypeDef o={0};RCC_ClkInitTypeDef c={0};
    o.OscillatorType=RCC_OSCILLATORTYPE_MSI;o.MSIState=RCC_MSI_ON;
    o.MSIClockRange=RCC_MSIRANGE_4;o.MSICalibrationValue=RCC_MSICALIBRATION_DEFAULT;o.PLL.PLLState=RCC_PLL_ON;o.PLL.PLLSource=RCC_PLLSOURCE_MSI;
    o.PLL.PLLMBOOST=RCC_PLLMBOOST_DIV1;o.PLL.PLLM=1;o.PLL.PLLN=80;
    o.PLL.PLLR=2;o.PLL.PLLP=2;o.PLL.PLLQ=2;o.PLL.PLLRGE=RCC_PLLVCIRANGE_0;
    if(HAL_RCC_OscConfig(&o)!=HAL_OK) Error_Handler();
    c.ClockType=RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2|RCC_CLOCKTYPE_PCLK3;
    c.SYSCLKSource=RCC_SYSCLKSOURCE_PLLCLK;c.AHBCLKDivider=RCC_SYSCLK_DIV1;
    c.APB1CLKDivider=c.APB2CLKDivider=c.APB3CLKDivider=RCC_HCLK_DIV1;
    if(HAL_RCC_ClockConfig(&c,FLASH_LATENCY_4)!=HAL_OK)Error_Handler();
}
static void uart_init(void) {
    __HAL_RCC_GPIOA_CLK_ENABLE();__HAL_RCC_GPIOC_CLK_ENABLE();__HAL_RCC_USART1_CLK_ENABLE();
    GPIO_InitTypeDef g={.Pin=GPIO_PIN_9|GPIO_PIN_10,.Mode=GPIO_MODE_AF_PP,.Pull=GPIO_PULLUP,.Speed=GPIO_SPEED_FREQ_HIGH,.Alternate=GPIO_AF7_USART1};
    HAL_GPIO_Init(GPIOA,&g);
    g.Pin=GPIO_PIN_13;g.Mode=GPIO_MODE_INPUT;g.Pull=GPIO_PULLDOWN;HAL_GPIO_Init(GPIOC,&g);
    uart.Instance=USART1;uart.Init.BaudRate=921600;uart.Init.WordLength=UART_WORDLENGTH_8B;
    uart.Init.StopBits=UART_STOPBITS_1;uart.Init.Parity=UART_PARITY_NONE;uart.Init.Mode=UART_MODE_TX_RX;
    uart.Init.HwFlowCtl=UART_HWCONTROL_NONE;uart.Init.OverSampling=UART_OVERSAMPLING_16;
    uart.Init.ClockPrescaler=UART_PRESCALER_DIV1;
    if(HAL_UART_Init(&uart)!=HAL_OK)Error_Handler();
}
static bool vectors_valid(uint32_t size,uint32_t id) {
    if(size<0x410 || size>DEVICE_APP_MAX || !device_app_id_valid(id))return false;
    const uint32_t *v=(const uint32_t *)DEVICE_APP_BASE;
    const device_app_info_t *info=(const device_app_info_t *)(DEVICE_APP_BASE+0x400);
    return v[0]>=0x20000000U && v[0]<=DEVICE_RAM_END && !(v[0]&7U) && (v[1]&1) &&
       (v[1]&~1U)>=DEVICE_APP_BASE && (v[1]&~1U)<DEVICE_APP_BASE+size &&
       info->magic==DEVICE_APP_MAGIC && info->id==id && info->protocol==DEVICE_PROTOCOL;
}
static bool app_valid(void) {
    const device_meta_t *m=(const device_meta_t *)DEVICE_META_BASE;
    return m->magic==DEVICE_META_MAGIC && vectors_valid(m->size,m->id) && checksum((const void *)DEVICE_APP_BASE,m->size)==m->crc;
}
static bool erase_range(uint32_t start,uint32_t bytes) {
    /* Never permit writes to bootloader or journal; bank swap is unsupported. */
    if(start<DEVICE_META_BASE || start>=DEVICE_STORE_BASE || !bytes || bytes>DEVICE_STORE_BASE-start || (start&8191))return false;
    uint32_t end=(start+bytes+8191U)&~8191U;
    if(end>DEVICE_STORE_BASE)return false;
    if(HAL_FLASH_Unlock()!=HAL_OK)return false;
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    bool ok=true;
    while(start<end && ok) {
        uint32_t bankbase=start<0x08200000U?DEVICE_BOOT_BASE:0x08200000U;
        uint32_t limit=bankbase+0x200000U;
        uint32_t stop=end<limit?end:limit,error;
        FLASH_EraseInitTypeDef e={.TypeErase=FLASH_TYPEERASE_PAGES,.Banks=bankbase==DEVICE_BOOT_BASE?FLASH_BANK_1:FLASH_BANK_2,
            .Page=(start-bankbase)/8192U,.NbPages=(stop-start)/8192U};
        ok=HAL_FLASHEx_Erase(&e,&error)==HAL_OK;start=stop;
    }
    HAL_FLASH_Lock();return ok;
}
static bool program(uint32_t address,const uint8_t *data,uint32_t size) {
    if(address<DEVICE_META_BASE || address>=DEVICE_STORE_BASE || size>DEVICE_STORE_BASE-address || (address&15) || (size&15))return false;
    if(HAL_FLASH_Unlock()!=HAL_OK)return false;
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    bool ok=true;
    for(uint32_t o=0;o<size && ok;o+=16)ok=HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD,address+o,(uint32_t)(data+o))==HAL_OK;
    HAL_FLASH_Lock();return ok && !memcmp((const void *)address,data,size);
}
/* The application always starts from a fresh reset: nothing this boot set up
 * (NOR port and pins, clocks, UART) reaches it. The flag survives the reset in
 * RAM; the next boot jumps before initialising anything. */
static void launch_app(void) __attribute__((noreturn));
static void launch_app(void) {
    DEVICE_MAILBOX[2]=DEVICE_LAUNCH_MAGIC;
    if(nor_ready)am_nor_deinit();
    HAL_UART_DeInit(&uart);
    __DSB();NVIC_SystemReset();
    while(1){}
}
static void start_app(void) __attribute__((noreturn));
static void start_app(void) {
    uint32_t sp=*(const uint32_t *)DEVICE_APP_BASE,pc=*(const uint32_t *)(DEVICE_APP_BASE+4);
    __disable_irq();SCB->VTOR=DEVICE_APP_BASE;
    __DSB();__ISB();
    /* A naked assembly tail avoids C stack accesses after changing MSP. */
    __asm volatile("msr control, %0\n msr msp, %1\n isb\n cpsie i\n bx %2"::"r"(0U),"r"(sp),"r"(pc):"memory");
    __builtin_unreachable();
}
static void transmit_reply(void) {
    for(uint32_t i=0;i<last_reply_size;++i)HAL_UART_Transmit(&uart,last_response+i,1,100);
}
static void reply(uint32_t seq,uint32_t cmd,uint32_t request_crc,uint32_t status,const char *json) {
    uint32_t h[5]={0x31544244U,seq,cmd|0x80000000U,4U+(uint32_t)strlen(json),0};
    memcpy(last_response,h,20);memcpy(last_response+20,&status,4);memcpy(last_response+24,json,strlen(json));
    h[4]=~device_crc_update(device_crc_update(0xFFFFFFFFU,(uint8_t *)h,16),last_response+20,h[3]);
    memcpy(last_response,h,20);last_reply_size=20+h[3];last_seq=seq;last_request_crc=request_crc;have_reply=true;
    transmit_reply();
}
static void dispatch(uint32_t *h) {
    uint32_t cmd=h[2],n=h[3],status=0;char out[240]="{}";
    if(cmd==1 || cmd==6) {
        const device_meta_t *m=(const device_meta_t *)DEVICE_META_BASE;
        snprintf(out,sizeof out,"{\"app\":\"DeviceBoot\",\"version\":4,\"protocol\":1,\"base\":%lu,\"max\":%lu,\"valid\":%s,\"image_id\":%lu,\"written\":%lu,\"max_app_id\":%lu,\"catalog\":%s,\"launching\":%s}",
          (unsigned long)DEVICE_APP_BASE,(unsigned long)DEVICE_APP_MAX,app_valid()?"true":"false",(unsigned long)(m->magic==DEVICE_META_MAGIC?m->id:0),(unsigned long)written,(unsigned long)DEVICE_APP_ID_MAX,nor_ready?"true":"false",launching?"true":"false");
    } else if(cmd==2) {
        if(n!=12)status=1;
        else {
            uint32_t p[3];memcpy(p,payload,12);
            if(p[0]<0x410 || p[0]>DEVICE_APP_MAX || !device_app_id_valid(p[2]))status=2;
            /* An independent upload must not leave an old install pending. */
            else if(!nor_ready || am_cancel(&am_stm32_io)<0)status=3;
            else {
                storing=false;
                updating=false;written=0;
                /* Invalidate BEFORE touching application pages. Interrupted uploads stay in recovery. */
                if(!erase_range(DEVICE_META_BASE,8192) || !erase_range(DEVICE_APP_BASE,p[0]))status=3;
                else {image_size=p[0];image_crc=p[1];image_id=p[2];updating=true;}
            }
        }
    } else if(cmd==3) {
        uint32_t offset=0;if(n>=4)memcpy(&offset,payload,4);
        uint32_t bytes=n>=4?n-4:0;
        if(!updating || !bytes || bytes>1024 || offset!=written || bytes>image_size-written ||
           (offset&15) || ((bytes&15) && offset+bytes!=image_size))status=4;
        else {
            uint32_t aligned=(bytes+15)&~15U;
            memmove(payload,payload+4,bytes);memset(payload+bytes,255,aligned-bytes);
            if(!program(DEVICE_APP_BASE+offset,payload,aligned)){status=3;updating=false;}
            else written+=bytes;
        }
        snprintf(out,sizeof out,"{\"written\":%lu}",(unsigned long)written);
    } else if(cmd==4) {
        if(!updating || written!=image_size || !vectors_valid(image_size,image_id) || checksum((void *)DEVICE_APP_BASE,image_size)!=image_crc)status=5;
        else {
            device_meta_t m __attribute__((aligned(16)))={DEVICE_META_MAGIC,image_size,image_crc,image_id};
            if(!program(DEVICE_META_BASE,(uint8_t *)&m,16))status=3;
            else strcpy(out,"{\"verified\":true}");
        }
        updating=false;
    } else if(cmd==5) {
        if(updating || !app_valid())status=6;
        else {reply(h[1],cmd,h[4],0,"{\"starting\":true}");HAL_Delay(30);launch_app();}
    } else if(cmd==7) {updating=false;storing=false;strcpy(out,"{\"aborted\":true}");}
    else if(cmd==8) {
        uint32_t slot=AM_SLOT_COUNT;am_header_t h;
        if(n==4)memcpy(&slot,payload,4);
        if(!nor_ready || am_read_header(&am_stm32_io,slot,&h)!=AM_OK)status=2;
        else snprintf(out,sizeof out,"{\"slot\":%lu,\"id\":%lu,\"version\":%lu,\"size\":%lu,\"crc\":%lu}",
          (unsigned long)slot,(unsigned long)h.id,(unsigned long)h.version,(unsigned long)h.size,(unsigned long)h.image_crc);
    } else if(cmd==9) {
        uint32_t p[5]={0};
        if(n==48)memcpy(p,payload,20);
        if(!nor_ready || n!=48 || p[0]>=AM_SLOT_COUNT || p[1]<0x410 || p[1]>AM_APP_MAX ||
           !device_app_id_valid(p[3]) || !p[4] || !memchr(payload+20,0,28))status=2;
        else if(updating || am_cancel(&am_stm32_io)<0)status=4;
        else {
            storing=false;store_slot=p[0];store_written=0;
            store_header=(am_header_t){.magic=AM_HEADER_MAGIC,.format=1,.id=p[3],.version=p[4],
                .size=p[1],.image_crc=p[2],.base=AM_APP_BASE,.protocol=1};
            memcpy(store_header.name,payload+20,28);
            /* Invalidate the old catalog header before altering its image. */
            uint32_t end=AM_IMAGE_OFFSET+((p[1]+4095u)&~4095u);
            for(uint32_t offset=0;offset<end && !status;offset+=4096)
                if(am_nor_erase4k(store_slot*AM_SLOT_SIZE+offset))status=3;
            storing=status==0;
        }
    } else if(cmd==10) {
        uint32_t offset=0;if(n>=4)memcpy(&offset,payload,4);
        uint32_t bytes=n>=4?n-4:0;
        if(!storing || !bytes || bytes>1024 || offset!=store_written || bytes>store_header.size-store_written)status=4;
        else if(am_nor_program(store_slot*AM_SLOT_SIZE+AM_IMAGE_OFFSET+offset,payload+4,bytes)){status=3;storing=false;}
        else store_written+=bytes;
        snprintf(out,sizeof out,"{\"written\":%lu}",(unsigned long)store_written);
    } else if(cmd==11) {
        if(n || !storing || store_written!=store_header.size)status=4;
        else {
            uint32_t crc=0xFFFFFFFFu;
            for(uint32_t offset=0;offset<store_written && !status;){
                uint32_t bytes=store_written-offset;if(bytes>sizeof payload)bytes=sizeof payload;
                if(am_nor_read(store_slot*AM_SLOT_SIZE+AM_IMAGE_OFFSET+offset,payload,bytes))status=3;
                else crc=device_crc_update(crc,payload,bytes);
                offset+=bytes;
            }
            if(!status && ~crc!=store_header.image_crc)status=5;
            store_header.header_crc=am_crc32(&store_header,60);
            if(!status && am_nor_program(store_slot*AM_SLOT_SIZE,&store_header,sizeof store_header))status=3;
            am_header_t check;
            if(!status && am_validate_slot(&am_stm32_io,store_slot,&check)!=AM_OK){
                status=5;(void)am_nor_erase4k(store_slot*AM_SLOT_SIZE);
            }
            if(!status)strcpy(out,"{\"verified\":true}");
        }
        storing=false;
    } else if(cmd==12) {
        uint32_t slot=AM_SLOT_COUNT;if(n==4)memcpy(&slot,payload,4);
        if(updating || storing)status=4;
        else if(!nor_ready || n!=4 || am_request(&am_stm32_io,slot)!=AM_OK)status=5;
        else {reply(h[1],cmd,h[4],0,"{\"installing\":true}");HAL_Delay(30);NVIC_SystemReset();}
    }
    else status=1;
    reply(h[1],cmd,h[4],status,out);
}
static bool packet(void) {
    static unsigned match;const uint8_t magic[4]={'D','B','T','1'};uint8_t b;
    if(HAL_UART_Receive(&uart,&b,1,10)!=HAL_OK){__HAL_UART_CLEAR_OREFLAG(&uart);return false;}
    match=b==magic[match]?match+1:(b==magic[0]?1:0);
    if(match!=4)return false;
    match=0;
    uint32_t h[5]={0x31544244U,0,0,0,0};
    if(HAL_UART_Receive(&uart,(uint8_t *)&h[1],16,500)!=HAL_OK)return false;
    if(h[3]>sizeof payload)return false;
    if(h[3] && HAL_UART_Receive(&uart,payload,h[3],1500)!=HAL_OK)return false;
    uint32_t crc=~device_crc_update(device_crc_update(0xFFFFFFFFU,(uint8_t *)h,16),payload,h[3]);
    if(crc!=h[4])return false;
    handled=h[2];
    if(have_reply && h[1]==last_seq && h[4]==last_request_crc)transmit_reply();
    else dispatch(h);
    return true;
}
static bool vectors_present(void) {
    const device_meta_t *m=(const device_meta_t *)DEVICE_META_BASE;
    return m->magic==DEVICE_META_MAGIC && vectors_valid(m->size,m->id);
}
/* Green LED (PE0) blinks while an application is copied from NOR. */
static void led_init(void) {
    __HAL_RCC_GPIOE_CLK_ENABLE();
    GPIO_InitTypeDef g={.Pin=GPIO_PIN_0,.Mode=GPIO_MODE_OUTPUT_PP,.Pull=GPIO_NOPULL,.Speed=GPIO_SPEED_FREQ_LOW};
    HAL_GPIO_Init(GPIOE,&g);
}
void am_port_progress(void) {
    static uint32_t last;
    if(HAL_GetTick()-last>=150){last=HAL_GetTick();HAL_GPIO_TogglePin(GPIOE,GPIO_PIN_0);}
}
int main(void) {
    /* Second half of a launch: the previous boot did its work and reset. */
    if(DEVICE_MAILBOX[2]==DEVICE_LAUNCH_MAGIC) {
        DEVICE_MAILBOX[2]=0;
        if(vectors_present())start_app();
    }
    bool requested=DEVICE_MAILBOX[0]==DEVICE_REQUEST_MAGIC && DEVICE_MAILBOX[1]==~DEVICE_REQUEST_MAGIC;
    DEVICE_MAILBOX[0]=DEVICE_MAILBOX[1]=0;
    HAL_Init();clock_init();crc_init();uart_init();
    bool user=HAL_GPIO_ReadPin(GPIOC,GPIO_PIN_13)==GPIO_PIN_SET;
    nor_ready=am_nor_init()==0;
    /* USER/BOOT recovery deliberately bypasses pending installation. A failed
     * resume never strands a valid application: it starts, and the pending
     * request is retried on the next boot. */
    if(nor_ready && !requested && !user) {
        led_init();
        int resumed=am_resume(&am_stm32_io);
        HAL_GPIO_WritePin(GPIOE,GPIO_PIN_0,GPIO_PIN_SET);
        const device_meta_t *m=(const device_meta_t *)DEVICE_META_BASE;
        if((resumed==AM_OK || resumed==AM_ALREADY_INSTALLED) && m->magic==DEVICE_META_MAGIC)
            DEVICE_MAILBOX[3]=device_opened_token(m->id);
    }
    bool stay=requested || user || !app_valid();
    /* A HELLO/STATUS probe (a PC connecting while the board powers up) is
     * answered with launching=true and never holds the application back;
     * only real bootloader work keeps this window open. */
    launching=!stay;
    uint32_t start=HAL_GetTick();
    while(!stay && HAL_GetTick()-start<900){
        if(packet() && handled!=1 && handled!=6)stay=true;
        if(HAL_GPIO_ReadPin(GPIOC,GPIO_PIN_13)==GPIO_PIN_SET)stay=true;
    }
    launching=false;
    if(!stay)launch_app();
    while(1)packet();
}
