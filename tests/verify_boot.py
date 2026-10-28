"""Run the actual DeviceBoot ARM ELF with simulated UART and Flash HAL calls.

Requires unicorn and pyelftools (also looks in the sibling SDK simulator-libs).
No hardware access. Run after python3 build.py.
"""
import argparse
import json
from pathlib import Path
import struct
import sys
import zlib

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--smartwatch-root',type=Path)
a=p.parse_args()
candidates=[a.smartwatch_root] if a.smartwatch_root else [ROOT.parent/name for name in ('smart_watch-main','Smartwatch','smart_watch')]
sdk=next((path for path in candidates if path and (path/'Core/Inc/device_runtime.h').is_file()),None)
if sdk:sys.path.insert(0,str(sdk/'tmp/simulator-libs'))
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import UC_CPU_ARM_CORTEX_M33, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR
from elftools.elf.elffile import ELFFile

APP=0x0800A000;STORE=0x083FC000;META=0x08008000;MAX_ID=0xFFFFFFFE
u=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS);u.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M33)
for address,size in [(0x08000000,0x400000),(0x20000000,0x280000),(0x40000000,0x10000000),(0xe000e000,0x2000)]:u.mem_map(address,size)
u.mem_write(0x08000000,b'\xff'*0x400000)
with (ROOT/'build/DeviceBoot.elf').open('rb') as source:
    elf=ELFFile(source)
    symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name}
    for segment in elf.iter_segments():
        if segment['p_type']=='PT_LOAD':u.mem_write(segment['p_vaddr'],segment.data())
u.mem_write(STORE,b'J'*16384)
saved_journal=bytes(u.mem_read(STORE,16384));saved_boot=bytes(u.mem_read(0x08000000,32768))
wire=bytearray();output=bytearray();erases=[];programs=[];waiting=False;started=False

def reg(index):return u.reg_read((UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)[index])
def ret(value=0):u.reg_write(UC_ARM_REG_R0,value);u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR))
def hook(name,fn):
    address=symbols[name]&~1
    u.hook_add(UC_HOOK_CODE,lambda *args:fn(),begin=address,end=address)
def receive():
    global waiting
    count=reg(2)
    if len(wire)<count:
        waiting=True;u.emu_stop();return
    u.mem_write(reg(1),bytes(wire[:count]));del wire[:count];ret()
def transmit():output.extend(u.mem_read(reg(1),reg(2)));ret()
def erase():
    _,bank,page,count=struct.unpack('<4I',u.mem_read(reg(0),16))
    address=(0x08000000 if bank==1 else 0x08200000)+page*8192
    assert META<=address and address+count*8192<=STORE,'Erase escaped allowed region'
    erases.append((address,count));u.mem_write(address,b'\xff'*(count*8192));ret()
def program():
    address=reg(1);assert META<=address and address+16<=STORE
    programs.append(address);u.mem_write(address,bytes(u.mem_read(reg(2),16)));ret()
for name in ('HAL_Init','HAL_PWREx_ControlVoltageScaling','HAL_PWREx_ConfigSupply','HAL_RCC_OscConfig',
             'HAL_RCC_ClockConfig','HAL_GPIO_Init','HAL_UART_Init','HAL_FLASH_Unlock','HAL_FLASH_Lock',
             'HAL_UART_DeInit','HAL_RCC_DeInit','HAL_DeInit','HAL_Delay'):
    if name in symbols:hook(name,ret)
hook('HAL_GPIO_ReadPin',lambda:ret(1)) # USER pressed: recovery, no automatic jump.
hook('HAL_UART_Receive',receive);hook('HAL_UART_Transmit',transmit)
hook('HAL_FLASHEx_Erase',erase);hook('HAL_FLASH_Program',program)
if 'Error_Handler' in symbols:
    hook('Error_Handler',lambda:(_ for _ in ()).throw(AssertionError('Bootloader Error_Handler')))
def start_app(*args):
    global started
    started=True;u.emu_stop()
u.hook_add(UC_HOOK_CODE,start_app,begin=APP+0x420,end=APP+0x420)
u.reg_write(UC_ARM_REG_SP,0x2026fff0);u.reg_write(UC_ARM_REG_LR,0x0fff0001)

def advance(pc):
    global waiting
    waiting=False;u.emu_start(pc|1,0,count=6000000)
    assert waiting or started,'Emulation did not reach UART wait or application entry'
advance(symbols['main'])
sequence=0;checks=[]
def check(condition,message):
    assert condition,message
    checks.append(message);print('PASS: '+message,flush=True)
def frame(command,payload=b''):
    global sequence
    sequence+=1;prefix=struct.pack('<4sIII',b'DBT1',sequence,command,len(payload))
    return prefix+struct.pack('<I',zlib.crc32(prefix+payload))+payload
def transact(raw,command):
    wire.extend(raw);output.clear();advance(u.reg_read(UC_ARM_REG_PC))
    assert len(output)>=24,'Missing reply'
    magic,seq,cmd,size,crc=struct.unpack_from('<4s4I',output)
    assert magic==b'DBT1' and seq==struct.unpack_from('<I',raw,4)[0] and cmd==command|0x80000000
    assert len(output)==20+size and crc==zlib.crc32(output[:16]+output[20:]),'Invalid reply framing/CRC'
    return struct.unpack_from('<I',output,20)[0],json.loads(output[24:])
def request(command,payload=b''):return transact(frame(command,payload),command)
status,hello=request(1)
check(status==0 and hello['version']==2 and hello['protocol']==1 and hello['max_app_id']==MAX_ID,'Revision 2 advertises extensible IDs on protocol 1')
for app_id in (0,0xffffffff):
    before=len(erases);status,_=request(2,struct.pack('<3I',2048,0,app_id))
    check(status==2 and len(erases)==before,f'Reserved ID {app_id} rejected before erase')
for size in (0,STORE-APP+1):
    before=len(erases);status,_=request(2,struct.pack('<3I',size,0,3))
    check(status==2 and len(erases)==before,f'Invalid size {size} rejected before erase')

def image(app_id):
    data=bytearray(2048);struct.pack_into('<2I',data,0,0x2026fff0,APP+0x421)
    struct.pack_into('<4I',data,0x400,0x44415631,app_id,7,1)
    return bytes(data)
def begin(app_id,data):return request(2,struct.pack('<3I',len(data),zlib.crc32(data),app_id))
def upload_blocks(data):
    for offset in range(0,len(data),1024):
        raw=frame(3,struct.pack('<I',offset)+data[offset:offset+1024])
        status,result=transact(raw,3)
        assert status==0 and result['written']==offset+1024
        before=len(programs);retry=transact(raw,3)
        assert retry==(status,result) and len(programs)==before,'Retry wrote Flash again'
for app_id in (1,2,3,MAX_ID):
    data=image(app_id)
    check(begin(app_id,data)[0]==0,f'BEGIN accepts application ID {app_id}')
    check(request(5)[0]==6,'Incomplete image cannot RUN')
    upload_blocks(data)
    check(request(4)[0]==0,f'FINISH commits validated application ID {app_id}')
    status,result=request(6)
    check(status==0 and result['valid'] and result['image_id']==app_id,'Committed image validates on STATUS')
data=image(4);begin(3,data);upload_blocks(data)
check(request(4)[0]==5 and request(5)[0]==6,'Mismatched descriptor cannot commit or RUN')
data=image(3);begin(3,data);request(7)
check(request(5)[0]==6,'Aborted upload cannot RUN')
begin(3,data);upload_blocks(data);request(4)
status,_=request(5)
check(status==0 and started,'RUN reaches third application reset vector')
check(bytes(u.mem_read(STORE,16384))==saved_journal,'16 KiB settings journal preserved')
check(bytes(u.mem_read(0x08000000,32768))==saved_boot,'Bootloader Flash preserved')
report={'scope':'ARM ELF simulation; UART/Flash HAL mocked; no physical board',
        'passed':len(checks),'failed':0,'checks':checks}
(ROOT/'build/verification-local.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'{len(checks)} ARM bootloader checks passed.')
