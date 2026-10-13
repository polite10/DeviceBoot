#ifndef DEVICE_CONTRACT_H
#define DEVICE_CONTRACT_H
#include <stdint.h>
#define DEVICE_BOOT_BASE 0x08000000U
#define DEVICE_BOOT_INFO 0x08007F00U
#define DEVICE_META_BASE 0x08008000U
#define DEVICE_APP_BASE  0x0800A000U
#define DEVICE_STORE_BASE 0x083FC000U
#define DEVICE_APP_MAX (DEVICE_STORE_BASE-DEVICE_APP_BASE)
#define DEVICE_RAM_END 0x2026FFF0U
#define DEVICE_MAILBOX ((volatile uint32_t *)DEVICE_RAM_END)
#define DEVICE_BOOT_MAGIC 0x44425631U
#define DEVICE_APP_MAGIC 0x44415631U
#define DEVICE_META_MAGIC 0x444D5631U
#define DEVICE_REQUEST_MAGIC 0xB00710ADU
#define DEVICE_PROTOCOL 1U
#define DEVICE_APP_WATCH 1U
#define DEVICE_APP_DUELLO 2U
typedef struct { uint32_t magic,version,base,max_size,protocol,board,reserved[2]; } device_boot_info_t;
typedef struct { uint32_t magic,id,version,protocol; } device_app_info_t;
typedef struct { uint32_t magic,size,crc,id; } device_meta_t;
static inline int device_boot_installed(void) {
    const device_boot_info_t *p=(const device_boot_info_t *)DEVICE_BOOT_INFO;
    return p->magic==DEVICE_BOOT_MAGIC && p->base==DEVICE_APP_BASE && p->protocol==DEVICE_PROTOCOL;
}
static inline void device_request_boot(void) {
    DEVICE_MAILBOX[0]=DEVICE_REQUEST_MAGIC;
    DEVICE_MAILBOX[1]=~DEVICE_REQUEST_MAGIC;
    __DSB(); NVIC_SystemReset();
}
#endif
