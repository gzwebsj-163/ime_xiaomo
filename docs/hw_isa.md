# xiaomo 硬件指令集参考 (Hardware ISA)

> 汇编级别控制硬件所需的完整指令集、寄存器布局、协议时序。
> 目标：让 xiaomo VM 能像裸机编程一样直接读/写硬件。

---

## 1. PCI 配置空间 (256 字节)

每个 PCI 设备有 256 字节配置空间，通过 `CONFIG_ADDRESS (0xCF8)` / `CONFIG_DATA (0xCFC)` IO 端口访问。

### 1.1 配置地址寄存器 (0xCF8, 32-bit)

```
Bit 31: Enable (1 = 访问配置空间)
Bit 30-24: 保留 (0)
Bit 23-16: Bus Number
Bit 15-11: Device Number
Bit 10-8:  Function Number
Bit 7-2:   Register Offset (双字对齐)
Bit 1-0:   00
```

### 1.2 标准 PCI 头 (Type 0, 非桥设备)

```
Offset  Size  Name
------  ----  ----
0x00    2B    Vendor ID           (只读)
0x02    2B    Device ID           (只读)
0x04    2B    Command             (R/W)
0x06    2B    Status              (R/W)
0x08    1B    Revision ID         (只读)
0x09    3B    Class Code          (只读)
0x0C    1B    Cache Line Size     (R/W)
0x0D    1B    Latency Timer       (R/W)
0x0E    1B    Header Type         (只读, bit7=1 多设备)
0x0F    1B    BIST               (R/W)
0x10    24B   BAR0-5              (R/W, 基址寄存器)
0x28    4B    Cardbus CIS Pointer
0x2C    2B    Subsystem Vendor ID (只读)
0x2E    2B    Subsystem ID        (只读)
0x30    4B    Expansion ROM Base  (R/W)
0x34    1B    Capabilities Ptr    (只读, 链表头)
0x35-3B 7B    保留
0x3C    1B    Interrupt Line      (R/W)
0x3D    1B    Interrupt PIN       (只读)
0x3E    1B    Min Grant           (只读)
0x3F    1B    Max Latency         (只读)
```

### 1.3 Command 寄存器 (0x04, 16-bit)

```
Bit 0:  IO Space Enable
Bit 1:  Memory Space Enable
Bit 2:  Bus Master Enable
Bit 3:  Special Cycles Enable
Bit 4:  Memory Write & Invalidate Enable
Bit 5:  VGA Palette Snoop
Bit 6:  Parity Error Response
Bit 7:  IDSEL Stepping/Wait Cycle
Bit 8:  SERR# Enable
Bit 9:  Fast Back-to-Back Enable
Bit 10: Interrupt Disable
```

### 1.4 Status 寄存器 (0x06, 16-bit)

```
Bit 3:  Interrupt Status
Bit 4:  Capabilities List
Bit 5:  66MHz Capable
Bit 7:  Fast Back-to-Back Capable
Bit 8:  Master Data Parity Error
Bit 11: Signalled Target Abort
Bit 12: Received Target Abort
Bit 13: Received Master Abort
Bit 14: Signalled System Error
Bit 15: Detected Parity Error
```

### 1.5 Class Code 表 (0x09, 3 字节)

```
Base Class:
  0x00  Unclassified
  0x01  Mass Storage (SCSI/IDE/SATA)
  0x02  Network (Ethernet/WiFi)
  0x03  Display (VGA/XGA/3D)
  0x04  Multimedia (Audio/Video)
  0x05  Memory (RAM/Flash)
  0x06  Bridge (Host/PCI/ISA)
  0x07  Simple Communication (Serial/Parallel)
  0x08  Base System Peripheral (PIC/Timer/DMA/RTC)
  0x09  Input (Keyboard/Mouse)
  0x0A  Docking Station
  0x0B  Processor (CPU/Co-processor)
  0x0C  Serial Bus (USB/FireWire/I²C)
  0x0D  Wireless
  0x0E  Intelligent IO
  0x0F  Satellite Communication
  0x10  Encryption/Decryption
  0x11  Signal Processing
  0x12  Processing Accelerator
  0x13  Non-Essential Instrumentation
  0x40  Co-Processor
  0xFF  Unassigned

Sub-Class (按 Base Class 不同):
  0x01,0x06: SATA AHCI
  0x02,0x00: Ethernet
  0x02,0x80: Network Other
  0x03,0x00: VGA Compatible
  0x03,0x80: Display Other
  0x04,0x03: HD Audio
  0x06,0x00: Host Bridge
  0x06,0x04: PCI-PCI Bridge
  0x06,0x01: ISA Bridge (LPC)
  0x0C,0x03: USB (UHCI/OHCI/EHCI/xHCI)
  0x0C,0x05: SMBus Controller
```

---

## 2. x86 IO 端口指令

### 2.1 指令格式

```
IN  AL,  imm8     ; 从端口 imm8 读 1 字节 → AL
IN  AX,  imm8     ; 从端口 imm8 读 2 字节 → AX
IN  EAX, imm8     ; 从端口 imm8 读 4 字节 → EAX
IN  AL,  DX       ; 从端口 DX 读 1 字节 → AL

OUT imm8, AL      ; 写 AL → 端口 imm8
OUT imm8, AX      ; 写 AX → 端口 imm8
OUT imm8, EAX     ; 写 EAX → 端口 imm8
OUT DX,   AL      ; 写 AL → 端口 DX
```

### 2.2 x86 标准 IO 端口表

```
端口范围    设备
--------    ----
0x000-0x01F  DMA Controller 1 (8237)
0x020-0x021  PIC 1 (8259A Master)
0x040-0x043  PIT (8253/8254 Timer)
0x060-0x064  Keyboard Controller (8042)
0x070-0x071  CMOS/RTC (NMI mask)
0x080-0x08F  DMA Page Register
0x0A0-0x0A1  PIC 2 (8259A Slave)
0x0C0-0x0DF  DMA Controller 2 (8237)
0x0F0-0x0FF  FPU / Math Coprocessor
0x170-0x177  Secondary IDE
0x1F0-0x1F7  Primary IDE
0x278-0x27F  Parallel Port 2 (LPT2)
0x2F8-0x2FF  Serial Port 2 (COM2)
0x378-0x37F  Parallel Port 1 (LPT1)
0x3B0-0x3BB  MDA/VGA Monochrome
0x3C0-0x3DF  VGA Color
0x3F0-0x3F7  Floppy Disk Controller
0x3F8-0x3FF  Serial Port 1 (COM1)
0xCF8-0xCFB  PCI Config Address
0xCFC-0xCFF  PCI Config Data
```

### 2.3 关键端口详解

**PIC (8259A)**
```
0x20  Master Command/Status
0x21  Master Data/Mask
0xA0  Slave Command/Status
0xA1  Slave Data/Mask

ICW1 (写 0x20, bit4=1):
  Bit 0: IC4=1 需要 ICW4
  Bit 1: SNGL=0 级联
  Bit 3: LTIM=1 电平触发
  Bit 4: 1=ICW1

OCW1 (IMR, 写 0x21/0xA1):
  每 bit 对应 IRQ: 1=屏蔽

EOI (写 0x20/0xA0):
  0x20 = 非特定 EOI
```

**PIT (8254)**
```
0x40  Channel 0 Data (系统时钟 IRQ0)
0x41  Channel 1 Data (DRAM 刷新)
0x42  Channel 2 Data (PC 扬声器)
0x43  Mode/Command Register

Command 字节:
  Bit 7-6: Channel (00=CH0, 01=CH1, 10=CH2, 11=Readback)
  Bit 5-4: Access (00=Latch, 01=Lo, 10=Hi, 11=LoHi)
  Bit 3-1: Mode (000=Mode0, 011=Mode3方波)
  Bit 0:   0=Binary, 1=BCD
```

**CMOS/RTC (0x70/0x71)**
```
0x70  Index Register (bit7=1 禁止 NMI)
0x71  Data Register

Index:
  0x00  Seconds
  0x02  Minutes
  0x04  Hours
  0x06  Day of Week
  0x07  Day of Month
  0x08  Month
  0x09  Year
  0x0A  Status A
  0x0B  Status B
  0x0C  Status C
  0x0D  Status D
  0x10  Floppy Type
  0x12  Hard Disk Type
  0x14  Equipment Byte
  0x15  Base Memory (Lo)
  0x16  Base Memory (Hi)
  0x17  Extended Memory (Lo)
  0x18  Extended Memory (Hi)
  0x2E-0x2F Checksum
```

**Keyboard Controller (8042)**
```
0x60  Data Port (Read/Write)
0x64  Status/Command Port

Status (读 0x64):
  Bit 0: Output Buffer Full
  Bit 1: Input Buffer Full
  Bit 2: System Flag
  Bit 3: Command/Data (0=Data, 1=Command)
  Bit 4: Inhibit Switch
  Bit 5: Aux Output Buffer Full
  Bit 6: General Purpose Timeout
  Bit 7: Parity Error

Command (写 0x64):
  0x20  Read Controller Command Byte
  0x60  Write Controller Command Byte
  0xA7  Disable Aux Port
  0xA8  Enable Aux Port
  0xA9  Interface Test
  0xAA  Controller Self-Test
  0xAB  Interface Test (Keyboard)
  0xAD  Disable Keyboard
  0xAE  Enable Keyboard
  0xD0  Read Output Port
  0xD1  Write Output Port
  0xD2  Write Keyboard Output Buffer
  0xD4  Write Aux (Mouse) Output Buffer
  0xF0-0xFF Pulse Output Bit
```

---

## 3. UART 16550 串口寄存器

### 3.1 寄存器布局 (基址 + 偏移)

```
DLAB=0 (LCR bit7=0):
  Offset 0  RBR (读) / THR (写)  Receiver Buffer / Transmitter Holding
  Offset 1  IER                  Interrupt Enable Register
  Offset 2  IIR (读) / FCR (写)  Interrupt Identification / FIFO Control
  Offset 3  LCR                  Line Control Register
  Offset 4  MCR                  Modem Control Register
  Offset 5  LSR                  Line Status Register
  Offset 6  MSR                  Modem Status Register
  Offset 7  SCR                  Scratch Register

DLAB=1 (LCR bit7=1):
  Offset 0  DLL                  Divisor Latch Low (波特率除数低字节)
  Offset 1  DLH                  Divisor Latch High (波特率除数高字节)
```

### 3.2 LCR (Line Control, Offset 3)

```
Bit 1-0: Word Length
  00 = 5 bits, 01 = 6 bits, 10 = 7 bits, 11 = 8 bits
Bit 2:   Stop Bits (0=1, 1=1.5/2)
Bit 3:   Parity Enable
Bit 4:   Even Parity Select (0=Odd, 1=Even)
Bit 5:   Stick Parity
Bit 6:   Set Break
Bit 7:   DLAB (Divisor Latch Access Bit)
```

### 3.3 LSR (Line Status, Offset 5)

```
Bit 0: Data Ready (DR)
Bit 1: Overrun Error (OE)
Bit 2: Parity Error (PE)
Bit 3: Framing Error (FE)
Bit 4: Break Interrupt (BI)
Bit 5: Transmitter Holding Register Empty (THRE)
Bit 6: Transmitter Empty (TEMT)
Bit 7: Error in RX FIFO
```

### 3.4 MCR (Modem Control, Offset 4)

```
Bit 0: DTR (Data Terminal Ready)
Bit 1: RTS (Request To Send)
Bit 2: OUT1 (Aux Output 1)
Bit 3: OUT2 (Aux Output 2, 使能中断)
Bit 4: Loopback Mode
```

### 3.5 波特率除数

```
除数 = 时钟频率 / (16 × 波特率)
标准时钟 = 115200 Hz

波特率   除数(DLH:DLL)
------   ------------
   50    0x0900
  300    0x0180
 1200    0x0060
 2400    0x0030
 4800    0x0018
 9600    0x000C
19200    0x0006
38400    0x0003
57600    0x0002
115200   0x0001
```

---

## 4. USB 协议 (硬件层)

### 4.1 USB 传输类型

```
Token PID:
  SETUP  0x2D (1101  = 0b1101, 反转 0b0010)
  IN     0x69 (1001  = 0b1001, 反转 0b0110)
  OUT    0xE1 (0001  = 0b0001, 反转 0b1110)

Data PID:
  DATA0  0xC3
  DATA1  0x4B
  DATA2  0x87
  MDATA  0x0F

Handshake PID:
  ACK    0xD2
  NAK    0x5A
  STALL  0x1E
  NYET   0x96
```

### 4.2 标准设备请求 (Setup Packet, 8 字节)

```
Offset  Field
------  -----
0       bmRequestType
         Bit 7:    方向 (0=Host>Device, 1=Device>Host)
         Bit 6-5:  类型 (0=Standard, 1=Class, 2=Vendor)
         Bit 4-0:  接收方 (0=Device, 1=Interface, 2=EP, 3=Other)
1       bRequest
         0x00 GET_STATUS
         0x01 CLEAR_FEATURE
         0x03 SET_FEATURE
         0x05 SET_ADDRESS
         0x06 GET_DESCRIPTOR
         0x07 SET_DESCRIPTOR
         0x08 GET_CONFIGURATION
         0x09 SET_CONFIGURATION
2-3     wValue (LE)
4-5     wIndex (LE)
6-7     wLength (LE)
```

### 4.3 描述符类型

```
0x01  Device Descriptor (18 B)
0x02  Configuration Descriptor (9 B)
0x03  String Descriptor
0x04  Interface Descriptor (9 B)
0x05  Endpoint Descriptor (7 B)
0x06  Device Qualifier
0x07  Other Speed Config
0x0F  BOS Descriptor
```

### 4.4 Device Descriptor (18 字节)

```
Offset  Size  Field
------  ----  -----
0       1B    bLength (18)
1       1B    bDescriptorType (0x01)
2       2B    bcdUSB (LE)
4       1B    bDeviceClass
5       1B    bDeviceSubClass
6       1B    bDeviceProtocol
7       1B    bMaxPacketSize0
8       2B    idVendor (LE)
10      2B    idProduct (LE)
12      2B    bcdDevice (LE)
14      1B    iManufacturer
15      1B    iProduct
16      1B    iSerialNumber
17      1B    bNumConfigurations
```

---

## 5. SMBus / I²C 协议

### 5.1 I²C 时序

```
START:  SDA↓ while SCL=1
STOP:   SDA↑ while SCL=1
DATA:   SDA stable during SCL=1 (change only when SCL=0)
ACK:    SDA=0 (Master releases, Slave pulls low) on 9th SCL

字节格式: S | ADDR+W | ACK | CMD | ACK | DATA | ACK | P
```

### 5.2 SMBus 命令 (Host Controller)

```
Intel PCH SMBus Base (PCI 00:1f.3, BAR 4-bit 0):
  Offset 0x00: HST_STS  (Host Status)
  Offset 0x02: HST_CNT  (Host Control)
  Offset 0x03: HST_CMD  (Host Command)
  Offset 0x04: XMIT_SLVA(Transmit Slave Address)
  Offset 0x05: HST_D0   (Host Data 0)
  Offset 0x06: HST_D1   (Host Data 1)
  Offset 0x07: BLOCK_DB (Block Data Byte)
  Offset 0x08: PEC      (Packet Error Check)
  Offset 0x09: RCV_SLVA (Receive Slave Address)
  Offset 0x0A: SLV_DATA (Slave Data)
```

---

## 6. Intel CPU MSR 寄存器 (Model-Specific Register)

### 6.1 读取指令

```
RDMSR:  ECX = MSR 地址 → EDX:EAX = 64-bit 值
WRMSR:  ECX = MSR 地址, EDX:EAX = 64-bit 值 → MSR
```

### 6.2 关键 MSR

```
0x0000001B  IA32_APIC_BASE          APIC 基址
0x0000002A  IA32_EBL_CR_POWERON     EBL CR Power-On
0x0000008B  IA32_BIOS_SIGN_ID       BIOS 签名
0x000000E7  IA32_MPERF              最大性能频率计数
0x000000E8  IA32_APERF              实际性能频率计数
0x000000FE  IA32_MTRRCAP            MTRR 能力
0x00000174  IA32_SYSENTER_CS        SYSENTER CS
0x00000175  IA32_SYSENTER_ESP       SYSENTER ESP
0x00000176  IA32_SYSENTER_EIP       SYSENTER EIP
0x00000179  IA32_MCG_CAP            机器检查能力
0x0000017A  IA32_MCG_STATUS         机器检查状态
0x0000019A  IA32_CLOCK_MODULATION   时钟调制
0x000001A0  IA32_MISC_ENABLE        杂项使能
0x000001A2  IA32_TEMPERATURE_TARGET 温度目标
0x000001A4  IA32_MISC_FEATURE       杂项特性
0x000001A6  IA32_OFFCORE_RSP_0      离线响应
0x000001A8  IA32_MISC_PWR_MGMT      杂项电源管理
0x000001AD  IA32_TURBO_RATIO_LIMIT  Turbo Ratio 限制
0x000001B0  IA32_ENERGY_PERF_BIAS   能效偏向
0x000001B1  IA32_PACKAGE_THERM_STATUS 封装温度状态
0x000001B2  IA32_PACKAGE_THERM_INTERRUPT 封装温度中断
0x000001FC  IA32_POWER_CTL          电源控制
0x00000200  IA32_MTRR_PHYSBASE0     MTRR 物理基址
0x00000277  IA32_PAT                Page Attribute Table
0x000002FF  IA32_MTRR_DEF_TYPE      MTRR 默认类型
0x00000309  IA32_FIXED_CTR0         固定计数器 0
0x00000345  IA32_PERF_CAPABILITIES  性能监测能力
0x0000038D  IA32_FIXED_CTR_CTRL     固定计数器控制
0x0000038E  IA32_PERF_GLOBAL_STATUS 性能全局状态
0x0000038F  IA32_PERF_GLOBAL_CTRL   性能全局控制
0x00000390  IA32_PERF_GLOBAL_OVF    性能全局溢出
0x000003F1  IA32_PEBS_ENABLE        PEBS 使能
0x000003F8  IA32_PKG_THERM_STATUS   封装温度状态
0x000003F9  IA32_PKG_THERM_INTERRUPT 封装温度中断
0x00000601  IA32_VMX_BASIC          VMX 基本能力
0x00000606  IA32_VMX_PROCBASED_CTLS VMX 处理器控制
0x0000060D  IA32_VMX_TRUE_PROCBASED VMX 真实处理器控制
0x0000060E  IA32_VMX_TRUE_EXIT      VMX 真实退出
0x0000060F  IA32_VMX_TRUE_ENTRY     VMX 真实进入
0x00000611  IA32_VMX_VMFUNC         VMX 功能
```

---

## 7. 内存映射 IO (MMIO) 区域

### 7.1 典型物理内存布局 (x86-64)

```
0x00000000-0x0009FFFF  常规内存 (640KB)
0x000A0000-0x000BFFFF  VGA 显存 (128KB)
0x000C0000-0x000FFFFF  BIOS ROM (256KB)
0x00100000-0x00EFFFFF  扩展内存 (< 15MB)
0x00F00000-0x00FFFFFF  系统 BIOS
0x01000000-0xFFFFFFFF  扩展内存 / PCI MMIO

以下为 PCI MMIO 范围 (物理地址):
0xF0000000-0xFEFFFFFF  通常为 PCI MMIO 窗口
0xFEC00000-0xFEC00FFF  IOAPIC
0xFED00000-0xFED003FF  HPET (高精度定时器)
0xFED90000-0xFED91FFF  TPM (if present)
0xFEE00000-0xFEE00FFF  LAPIC (本地 APIC)
0xFF000000-0xFFFFFFFF  BIOS ROM
```

### 7.2 LAPIC 寄存器 (基址 0xFEE00000)

```
Offset  Register
------  --------
0x0020  LAPIC ID
0x0030  LAPIC Version
0x0080  Task Priority
0x0090  Arbitration Priority
0x00A0  Processor Priority
0x00B0  EOI
0x00D0  Logical Destination
0x00E0  Destination Format
0x00F0  Spurious Interrupt Vector
0x0100-0170 ISR (In-Service Register)
0x0180-01F0 TMR (Trigger Mode Register)
0x0200-0270 IRR (Interrupt Request Register)
0x0280  Error Status
0x0300  Interrupt Command (Lo 32)
0x0310  Interrupt Command (Hi 32)
0x0320  LVT Timer
0x0330  LVT Thermal
0x0340  LVT Performance
0x0350  LVT LINT0
0x0360  LVT LINT1
0x0370  LVT Error
0x0380  Timer Initial Count
0x0390  Timer Current Count
0x03E0  Timer Divide Config
```

---

## 8. Intel Haswell-ULT (i5-4250U) 特定信息

### 8.1 PCH (Lynx Point-LP) 芯片组

```
Device: Intel 8 Series / C220 (Lynx Point-LP)
Vendor: 0x8086, Device: 0x9C43 (LPC)
SMBus:  PCI 00:1F.3, Device 0x8086:0x9C22

GPIO: 可通过 I/O 空间访问 (BAR 或通过 LPC I/O 解码)
PMBASE: ACPI I/O 基址 (通常 0x1800-0x18FF)
  PM1_STS:  PMBASE+0x00
  PM1_EN:   PMBASE+0x02
  PM1_CNT:  PMBASE+0x04
  PM1_TMR:  PMBASE+0x08
  GPE0_STS: PMBASE+0x20
  GPE0_EN:  PMBASE+0x28
```

### 8.2 HD Graphics 5000 (GT3)

```
PCI: 00:02.0, Vendor: 0x8086, Device: 0x0A26
MMIO BAR0: 64MB (GT 寄存器)
MMIO BAR2: 256MB (全局 GTT + 显存)

GT 寄存器 (部分):
  0x130040  GT_MAILBOX_DATA
  0x130044  GT_MAILBOX_INTERFACE
  0x138124  RP_CAP (Render Power Capability)
  0x138128  GT_PERF_STATUS
  0x138140  RC_CONTROL
  0x138150  GT_GFX_RP_MAIN
  0x138154  GT_GFX_RP_DOWN_TIMEOUT
  0x138158  GT_GFX_RP_INTERRUPT_LIMITS
  0x138160  GT_GFX_RP_DOWN_THRESHOLD
  0x138164  GT_GFX_RP_UP_THRESHOLD
  0x138168  GT_GFX_RP_DOWN_EI
  0x13816C  GT_GFX_RP_UP_EI
  0x138170  GT_GFX_RP_IDLE_HYSTERSIS
  0x138180  GT_GFX_RC6_RESIDENCY_TIME
  0x1381A0  GT_GFX_RP_POWER_CONTROL
```

---

## 9. xiaomo 硬件指令扩展 (规划)

### 9.1 新增 Kills Opcode

```
OP_HW_INB      读 IO 端口 → 寄存器
OP_HW_OUTB     写寄存器 → IO 端口
OP_HW_INL      读 IO 端口 32-bit
OP_HW_OUTL     写 IO 端口 32-bit
OP_HW_PCI_RD   读 PCI 配置空间
OP_HW_PCI_WR   写 PCI 配置空间
OP_HW_MMIO_RD  读 MMIO 地址
OP_HW_MMIO_WR  写 MMIO 地址
OP_HW_MSR_RD   读 MSR 寄存器
OP_HW_MSR_WR   写 MSR 寄存器
OP_HW_DEV_ENUM 枚举设备 (PCI/USB/Serial)
OP_HW_UART_CFG 配置串口
OP_HW_UART_RD  读串口
OP_HW_UART_WR  写串口
```

### 9.2 指令编码约定

```
OP_HW_INB:  a=dst_reg, b=port, imm=0
OP_HW_OUTB: a=port, b=src_reg, imm=0
OP_HW_PCI_RD: a=dst_reg, b=bus, imm=(dev<<16|func<<8|offset)
OP_HW_PCI_WR: a=bus, b=src_reg, imm=(dev<<16|func<<8|offset)
OP_HW_MMIO_RD: a=dst_reg, b=addr_reg, imm=width(1/2/4/8)
OP_HW_MSR_RD: a=dst_lo_reg, b=dst_hi_reg, imm=msr_addr
OP_HW_DEV_ENUM: a=type(0=PCI,1=USB,2=Serial), b=0, imm=0
```

---

## 参考

- PCI Local Bus Specification 3.0
- Intel 64 and IA-32 Architectures Software Developer's Manual
- 16550 UART Datasheet
- USB 2.0 Specification
- Intel 8 Series PCH Datasheet
- SMBus Specification 2.0