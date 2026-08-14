/*
 * xiaomo - 硬件直访模块 (Hardware Direct Access)
 *
 * 功能: 让 Kills VM 能通过指令直接访问底层硬件
 *   - IO 端口读写 (IN/OUT)
 *   - PCI 配置空间读写
 *   - 内存映射 IO (MMIO)
 *   - MSR 寄存器访问
 *   - 设备枚举 (PCI/USB/串口)
 *
 * 平台: macOS / x86-64
 * 依赖: IOKit (PCI), CoreFoundation (设备枚举)
 *
 * 安全: 所有操作需要 root 权限 (IO 端口 / MSR / MMIO)
 *       在 macOS 上 IO 端口和 MSR 需要内核扩展支持
 *       本模块提供用户态能用的最佳替代方案
 */

#include "hw_direct.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <dirent.h>
#include <string.h>

#if defined(__APPLE__)
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <CoreFoundation/CoreFoundation.h>

/* IOPCIDevice 属性常量 (直接定义, 避免依赖 Kernel.framework) */
#ifndef kIOPCIConfigSpace
#define kIOPCIConfigSpace "IODeviceMemory"
#endif
#endif

/* ===================== 辅助: 错误消息 ===================== */
static char hw_last_error[512] = "";
const char* hw_get_error(void) { return hw_last_error; }
static void hw_set_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(hw_last_error, sizeof(hw_last_error), fmt, ap);
    va_end(ap);
}

/* ===================== 1. IO 端口 ===================== */

/*
 * macOS 安全限制: 用户态不能直接执行 in/out 指令
 * 替代方案: 通过 /dev/io 或 iokit 驱动
 * 本实现提供框架，真实 IO 需要内核扩展
 *
 * 对于 macOS 上可用的设备，通过 IOKit 访问
 */

int hw_inb(uint16_t port, uint8_t* val) {
    (void)port;
    hw_set_error("IO port access not available on macOS (needs kext)");
    return -1;
}

int hw_outb(uint16_t port, uint8_t val) {
    (void)port; (void)val;
    hw_set_error("IO port access not available on macOS (needs kext)");
    return -1;
}

int hw_inl(uint16_t port, uint32_t* val) {
    (void)port;
    hw_set_error("IO port access not available on macOS (needs kext)");
    return -1;
}

int hw_outl(uint16_t port, uint32_t val) {
    (void)port; (void)val;
    hw_set_error("IO port access not available on macOS (needs kext)");
    return -1;
}

/* ===================== 2. PCI 配置空间 ===================== */

/*
 * macOS 通过 IOKit 访问 PCI 配置空间
 * 不需要直接操作 IO 端口 0xCF8/0xCFC
 */

#if defined(__APPLE__)

static io_connect_t g_pci_connect = 0;
static mach_port_t g_master_port = 0;

/* 获取 master port (懒初始化) */
static mach_port_t hw_get_master_port(void) {
    if (g_master_port == 0) {
        IOMasterPort(MACH_PORT_NULL, &g_master_port);
    }
    return g_master_port;
}

/* 获取 PCI 设备的 IOKit service */
static io_service_t hw_pci_get_service(uint8_t bus, uint8_t dev, uint8_t func) {
    CFMutableDictionaryRef match = IOServiceMatching("IOPCIDevice");
    if (!match) return 0;

    io_iterator_t iter = 0;
    kern_return_t kr = IOServiceGetMatchingServices(hw_get_master_port(), match, &iter);
    if (kr != KERN_SUCCESS) return 0;

    io_service_t service;
    while ((service = IOIteratorNext(iter))) {
        CFDataRef reg = (CFDataRef)IORegistryEntryCreateCFProperty(
            service, CFSTR("reg"), kCFAllocatorDefault, 0);
        if (reg) {
            const uint8_t* data = CFDataGetBytePtr(reg);
            CFIndex len = CFDataGetLength(reg);
            if (len >= 8) {
                uint32_t bdf = *(uint32_t*)(data);
                uint8_t b = (bdf >> 16) & 0xFF;
                uint8_t d = (bdf >> 11) & 0x1F;
                uint8_t f = (bdf >> 8) & 0x07;
                if (b == bus && d == dev && f == func) {
                    CFRelease(reg);
                    IOObjectRelease(iter);
                    return service;
                }
            }
            CFRelease(reg);
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);
    return 0;
}

/* 通过 IOKit 读 PCI 配置空间 */
static int hw_pci_read_iokit(io_service_t service, uint32_t offset,
                              uint32_t* val, int width) {
    if (!service) return -1;

    /* 构建 Memory Read 请求:
     * 通过 IOPCIDevice 的 configRead 方法访问
     * 或者直接读取 IORegistry 中的配置空间属性 */
    CFMutableDataRef configData = CFDataCreateMutable(kCFAllocatorDefault, 256);
    if (!configData) return -1;

    /* 尝试读取整个配置空间快照 */
    // 实际 macOS 不直接暴露用户态 config space 读写
    // 退而求其次: 通过 system_profiler / ioreg 的 Device ID 等属性
    // 这里我们直接读取已知属性, 对于完整 256B 配置空间,
    // 需要通过 PCIUserClient 或 DEXT 实现

    /* 简化: 只支持读取已知 IORegistry 属性 */
    char key[32];
    uint32_t result = 0xFFFFFFFF;

    switch (offset) {
    case 0x00: { /* Vendor + Device */
        CFDataRef vid = (CFDataRef)IORegistryEntryCreateCFProperty(
            service, CFSTR("vendor-id"), kCFAllocatorDefault, 0);
        CFDataRef did = (CFDataRef)IORegistryEntryCreateCFProperty(
            service, CFSTR("device-id"), kCFAllocatorDefault, 0);
        /* IOKit 返回的 vendor-id/device-id 是小端 uint32,
         * 低 16 位是实际值 */
        if (vid && CFDataGetLength(vid) >= 4) {
            uint32_t raw = *(uint32_t*)CFDataGetBytePtr(vid);
            result = (raw & 0xFFFF);  /* 低 16 位 = vendor */
        }
        if (did && CFDataGetLength(did) >= 4) {
            uint32_t raw = *(uint32_t*)CFDataGetBytePtr(did);
            result |= ((raw & 0xFFFF) << 16);  /* device 在高 16 位 */
        }
        if (vid) CFRelease(vid);
        if (did) CFRelease(did);
        break;
    }
    case 0x08: { /* Revision + Class */
        CFDataRef rid = (CFDataRef)IORegistryEntryCreateCFProperty(
            service, CFSTR("revision-id"), kCFAllocatorDefault, 0);
        CFDataRef cc = (CFDataRef)IORegistryEntryCreateCFProperty(
            service, CFSTR("class-code"), kCFAllocatorDefault, 0);
        if (rid && CFDataGetLength(rid) >= 4) {
            uint32_t raw = *(uint32_t*)CFDataGetBytePtr(rid);
            result = raw & 0xFF;  /* revision 在低 8 位 */
        }
        if (cc && CFDataGetLength(cc) >= 4) {
            uint32_t raw = *(uint32_t*)CFDataGetBytePtr(cc);
            result |= ((raw & 0xFFFFFF) << 8);  /* class code 3 字节 */
        }
        if (rid) CFRelease(rid);
        if (cc) CFRelease(cc);
        break;
    }
    case 0x2C: { /* Subsystem Vendor + Subsystem */
        CFDataRef svid = (CFDataRef)IORegistryEntryCreateCFProperty(
            service, CFSTR("subsystem-vendor-id"), kCFAllocatorDefault, 0);
        CFDataRef sid = (CFDataRef)IORegistryEntryCreateCFProperty(
            service, CFSTR("subsystem-id"), kCFAllocatorDefault, 0);
        if (svid && CFDataGetLength(svid) >= 4) {
            uint32_t raw = *(uint32_t*)CFDataGetBytePtr(svid);
            result = (raw & 0xFFFF);
        }
        if (sid && CFDataGetLength(sid) >= 4) {
            uint32_t raw = *(uint32_t*)CFDataGetBytePtr(sid);
            result = (result & 0xFFFF0000) | (raw & 0xFFFF);
        }
        if (svid) CFRelease(svid);
        if (sid) CFRelease(sid);
        break;
    }
    default:
        /* 其他偏移在 macOS 用户态不可直接访问
         * 需要 DEXT (DriverKit Extension) 或内核扩展
         * 返回 0xFFFFFFFF 表示不可读 */
        result = 0xFFFFFFFF;
        hw_set_error("PCI offset 0x%02X not available in userspace", offset);
        break;
    }

    CFRelease(configData);
    *val = result;
    return 0;
}
#endif

int hw_pci_read(uint8_t bus, uint8_t dev, uint8_t func,
                uint32_t offset, uint32_t* val, int width) {
#if defined(__APPLE__)
    io_service_t service = hw_pci_get_service(bus, dev, func);
    if (!service) {
        hw_set_error("PCI %02X:%02X.%X not found", bus, dev, func);
        return -1;
    }
    int rc = hw_pci_read_iokit(service, offset, val, width);
    IOObjectRelease(service);
    return rc;
#else
    (void)bus; (void)dev; (void)func; (void)offset; (void)width;
    hw_set_error("PCI access not implemented on this platform");
    return -1;
#endif
}

int hw_pci_write(uint8_t bus, uint8_t dev, uint8_t func,
                 uint32_t offset, uint32_t val, int width) {
    (void)bus; (void)dev; (void)func; (void)offset; (void)val; (void)width;
    hw_set_error("PCI write not available in userspace (needs DEXT)");
    return -1;
}

/* ===================== 3. MMIO 内存映射 IO ===================== */

/*
 * macOS 上可以通过 /dev/mem 或 IOMemoryDescriptor 访问 MMIO
 * 但 /dev/mem 在 macOS 上不可用。替代: IOKit IOServiceOpen
 *
 * 对于已知物理地址的 MMIO 范围:
 *   1. 通过 IOKit 找到对应设备
 *   2. 获取 IOMemoryMap
 *   3. 映射到用户空间
 */

static int hw_mmio_fd = -1;
static uint8_t* hw_mmio_base = NULL;
static uint64_t hw_mmio_phys = 0;
static uint64_t hw_mmio_size = 0;

int hw_mmio_map(uint64_t phys_addr, uint64_t size) {
    /* macOS 没有 /dev/mem, 需要通过 IOKit 映射
     * 这里提供框架, 实际使用需要配合 IOKit Memory Descriptor */
    (void)phys_addr; (void)size;
    hw_set_error("MMIO mapping not available on macOS (needs IOKit driver)");
    return -1;
}

int hw_mmio_unmap(void) {
    if (hw_mmio_base) {
        munmap(hw_mmio_base, hw_mmio_size);
        hw_mmio_base = NULL;
    }
    hw_mmio_phys = hw_mmio_size = 0;
    return 0;
}

int hw_mmio_read(uint64_t offset, uint64_t* val, int width) {
    if (!hw_mmio_base) {
        hw_set_error("MMIO not mapped");
        return -1;
    }
    if (offset + width > hw_mmio_size) {
        hw_set_error("MMIO access out of bounds");
        return -1;
    }
    switch (width) {
    case 1: *val = hw_mmio_base[offset]; break;
    case 2: *val = *(uint16_t*)(hw_mmio_base + offset); break;
    case 4: *val = *(uint32_t*)(hw_mmio_base + offset); break;
    case 8: *val = *(uint64_t*)(hw_mmio_base + offset); break;
    default: return -1;
    }
    return 0;
}

int hw_mmio_write(uint64_t offset, uint64_t val, int width) {
    if (!hw_mmio_base) {
        hw_set_error("MMIO not mapped");
        return -1;
    }
    if (offset + width > hw_mmio_size) {
        hw_set_error("MMIO access out of bounds");
        return -1;
    }
    switch (width) {
    case 1: hw_mmio_base[offset] = (uint8_t)val; break;
    case 2: *(uint16_t*)(hw_mmio_base + offset) = (uint16_t)val; break;
    case 4: *(uint32_t*)(hw_mmio_base + offset) = (uint32_t)val; break;
    case 8: *(uint64_t*)(hw_mmio_base + offset) = val; break;
    default: return -1;
    }
    return 0;
}

/* ===================== 4. MSR 寄存器 ===================== */

/*
 * macOS 用户态不能执行 RDMSR/WRMSR 指令
 * 需要内核扩展 (kext) 提供接口
 * 替代: 通过 IOKit 读取某些 CPU 信息
 */

int hw_msr_read(uint32_t msr_addr, uint64_t* val) {
    (void)msr_addr;
    hw_set_error("MSR access not available on macOS (needs kext)");
    return -1;
}

int hw_msr_write(uint32_t msr_addr, uint64_t val) {
    (void)msr_addr; (void)val;
    hw_set_error("MSR access not available on macOS (needs kext)");
    return -1;
}

/* ===================== 5. 设备枚举 ===================== */

/*
 * 枚举 PCI 设备列表
 * 输出格式: 每个设备一行 "bus:dev.func vendor:device class name"
 */
int hw_pci_enumerate(char* buf, int buflen) {
    if (!buf || buflen < 1) return -1;
    buf[0] = '\0';
    int off = 0;

#if defined(__APPLE__)
    CFMutableDictionaryRef match = IOServiceMatching("IOPCIDevice");
    if (!match) return -1;

    io_iterator_t iter = 0;
    kern_return_t kr = IOServiceGetMatchingServices(hw_get_master_port(), match, &iter);
    if (kr != KERN_SUCCESS) {
        hw_set_error("IOServiceGetMatchingServices failed: %d", kr);
        return -1;
    }

    io_service_t service;
    while ((service = IOIteratorNext(iter))) {
        /* 获取 BDF */
        CFDataRef reg = (CFDataRef)IORegistryEntryCreateCFProperty(
            service, CFSTR("reg"), kCFAllocatorDefault, 0);
        uint32_t bdf = 0;
        if (reg) {
            if (CFDataGetLength(reg) >= 4)
                bdf = *(uint32_t*)CFDataGetBytePtr(reg);
            CFRelease(reg);
        }
        uint8_t bus = (bdf >> 16) & 0xFF;
        uint8_t dev = (bdf >> 11) & 0x1F;
        uint8_t func = (bdf >> 8) & 0x07;

        /* Vendor / Device */
        CFDataRef vid = (CFDataRef)IORegistryEntryCreateCFProperty(
            service, CFSTR("vendor-id"), kCFAllocatorDefault, 0);
        CFDataRef did = (CFDataRef)IORegistryEntryCreateCFProperty(
            service, CFSTR("device-id"), kCFAllocatorDefault, 0);
        CFDataRef cc = (CFDataRef)IORegistryEntryCreateCFProperty(
            service, CFSTR("class-code"), kCFAllocatorDefault, 0);
        CFStringRef name = (CFStringRef)IORegistryEntryCreateCFProperty(
            service, CFSTR("IOName"), kCFAllocatorDefault, 0);

        uint32_t vendor = vid && CFDataGetLength(vid) >= 4 ?
            *(uint32_t*)CFDataGetBytePtr(vid) : 0xFFFF;
        uint32_t device = did && CFDataGetLength(did) >= 4 ?
            *(uint32_t*)CFDataGetBytePtr(did) : 0xFFFF;
        uint32_t class_code = cc && CFDataGetLength(cc) >= 4 ?
            *(uint32_t*)CFDataGetBytePtr(cc) : 0;

        char namebuf[64] = "?";
        if (name) {
            CFStringGetCString(name, namebuf, sizeof(namebuf),
                               kCFStringEncodingUTF8);
        }

        int n = snprintf(buf + off, buflen - off,
                         "%02X:%02X.%X %04X:%04X %06X %s\n",
                         bus, dev, func,
                         vendor & 0xFFFF, device & 0xFFFF,
                         class_code & 0xFFFFFF, namebuf);
        if (n > 0 && off + n < buflen) off += n;
        else break;

        if (vid) CFRelease(vid);
        if (did) CFRelease(did);
        if (cc) CFRelease(cc);
        if (name) CFRelease(name);
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);
#else
    snprintf(buf, buflen, "PCI enumeration not available on this platform\n");
#endif
    return off;
}

/*
 * 枚举 USB 设备
 */
int hw_usb_enumerate(char* buf, int buflen) {
    if (!buf || buflen < 1) return -1;
    buf[0] = '\0';
    int off = 0;

#if defined(__APPLE__)
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBDevice");
    if (!match) {
        /* 尝试新版 USB 驱动 */
        match = IOServiceMatching("IOUSBHostDevice");
    }
    if (!match) {
        hw_set_error("Cannot find USB device matching dictionary");
        return -1;
    }

    io_iterator_t iter = 0;
    kern_return_t kr = IOServiceGetMatchingServices(hw_get_master_port(), match, &iter);
    if (kr != KERN_SUCCESS) {
        hw_set_error("USB IOServiceGetMatchingServices failed: %d", kr);
        return -1;
    }

    io_service_t service;
    while ((service = IOIteratorNext(iter))) {
        CFNumberRef vid = (CFNumberRef)IORegistryEntryCreateCFProperty(
            service, CFSTR("idVendor"), kCFAllocatorDefault, 0);
        CFNumberRef pid = (CFNumberRef)IORegistryEntryCreateCFProperty(
            service, CFSTR("idProduct"), kCFAllocatorDefault, 0);
        CFStringRef pname = (CFStringRef)IORegistryEntryCreateCFProperty(
            service, CFSTR("USB Product Name"), kCFAllocatorDefault, 0);
        CFStringRef vname = (CFStringRef)IORegistryEntryCreateCFProperty(
            service, CFSTR("USB Vendor Name"), kCFAllocatorDefault, 0);

        uint16_t vendor = 0, product = 0;
        if (vid) { uint32_t tmp = 0; CFNumberGetValue(vid, kCFNumberSInt32Type, &tmp); vendor = tmp; }
        if (pid) { uint32_t tmp = 0; CFNumberGetValue(pid, kCFNumberSInt32Type, &tmp); product = tmp; }

        char pnamebuf[64] = "?", vnamebuf[64] = "?";
        if (pname) CFStringGetCString(pname, pnamebuf, sizeof(pnamebuf), kCFStringEncodingUTF8);
        if (vname) CFStringGetCString(vname, vnamebuf, sizeof(vnamebuf), kCFStringEncodingUTF8);

        int n = snprintf(buf + off, buflen - off,
                         "%04X:%04X %s / %s\n",
                         vendor, product, vnamebuf, pnamebuf);
        if (n > 0 && off + n < buflen) off += n;
        else break;

        if (vid) CFRelease(vid);
        if (pid) CFRelease(pid);
        if (pname) CFRelease(pname);
        if (vname) CFRelease(vname);
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);
#else
    snprintf(buf, buflen, "USB enumeration not available on this platform\n");
#endif
    return off;
}

/*
 * 枚举串口设备
 */
int hw_serial_enumerate(char* buf, int buflen) {
    if (!buf || buflen < 1) return -1;
    buf[0] = '\0';
    int off = 0;

    /* 直接扫描 /dev/cu.* */
    DIR* d = opendir("/dev");
    if (!d) {
        hw_set_error("Cannot open /dev");
        return -1;
    }

    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (strncmp(entry->d_name, "cu.", 3) == 0) {
            int n = snprintf(buf + off, buflen - off,
                             "/dev/%s\n", entry->d_name);
            if (n > 0 && off + n < buflen) off += n;
            else break;
        }
    }
    closedir(d);
    return off;
}

/* ===================== 6. CPU 信息 ===================== */

int hw_cpu_info(char* buf, int buflen) {
    if (!buf || buflen < 1) return -1;
    int off = 0;

    /* 读取 sysctl CPU 信息 */
    FILE* f = popen("sysctl -n machdep.cpu.brand_string hw.physicalcpu "
                    "hw.logicalcpu hw.memsize machdep.cpu.features "
                    "machdep.cpu.leaf7_features 2>/dev/null", "r");
    if (!f) return -1;

    char line[256];
    int line_n = 0;
    while (fgets(line, sizeof(line), f) && off < buflen - 1) {
        int n = 0;
        switch (line_n) {
        case 0: n = snprintf(buf + off, buflen - off, "CPU: %s", line); break;
        case 1: n = snprintf(buf + off, buflen - off, "Cores: %s", line); break;
        case 2: n = snprintf(buf + off, buflen - off, "Threads: %s", line); break;
        case 3: n = snprintf(buf + off, buflen - off, "RAM: %.1f GB\n",
                             atol(line) / (1024.0*1024*1024)); break;
        case 4: n = snprintf(buf + off, buflen - off, "Feat: %s", line); break;
        case 5: n = snprintf(buf + off, buflen - off, "Feat7: %s", line); break;
        }
        if (n > 0 && off + n < buflen) off += n;
        line_n++;
    }
    pclose(f);
    return off;
}

/* ===================== 7. UART 串口操作 ===================== */

/*
 * 串口操作 (通过 POSIX termios)
 * macOS 上串口表示为 /dev/cu.* 设备文件
 */

typedef struct {
    int fd;
    char path[256];
} HwUart;

static HwUart g_uarts[8];
static int g_uart_count = 0;

int hw_uart_open(const char* path, int baud) {
    if (g_uart_count >= 8) {
        hw_set_error("Max UART devices (8) reached");
        return -1;
    }

    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        hw_set_error("Cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    /* 配置 termios */
    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        hw_set_error("tcgetattr failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    cfsetospeed(&tty, (speed_t)baud);
    cfsetispeed(&tty, (speed_t)baud);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;  /* 8-bit */
    tty.c_cflag &= ~PARENB;  /* 无校验 */
    tty.c_cflag &= ~CSTOPB;  /* 1 stop bit */
    tty.c_cflag &= ~CRTSCTS; /* 无硬件流控 */
    tty.c_cflag |= CREAD | CLOCAL; /* 启用接收 */

    tty.c_iflag &= ~(IXON | IXOFF | IXANY); /* 无软件流控 */
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); /* 原始模式 */
    tty.c_oflag &= ~OPOST;

    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1; /* 100ms 超时 */

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        hw_set_error("tcsetattr failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    int idx = g_uart_count++;
    g_uarts[idx].fd = fd;
    snprintf(g_uarts[idx].path, sizeof(g_uarts[idx].path), "%s", path);
    return idx;
}

int hw_uart_close(int handle) {
    if (handle < 0 || handle >= g_uart_count) {
        hw_set_error("Invalid UART handle %d", handle);
        return -1;
    }
    close(g_uarts[handle].fd);
    g_uarts[handle].fd = -1;
    return 0;
}

int hw_uart_read(int handle, uint8_t* buf, int maxlen) {
    if (handle < 0 || handle >= g_uart_count) {
        hw_set_error("Invalid UART handle %d", handle);
        return -1;
    }
    int n = (int)read(g_uarts[handle].fd, buf, maxlen);
    if (n < 0 && errno != EAGAIN && errno != EINTR) {
        hw_set_error("UART read error: %s", strerror(errno));
        return -1;
    }
    return n < 0 ? 0 : n;
}

int hw_uart_write(int handle, const uint8_t* buf, int len) {
    if (handle < 0 || handle >= g_uart_count) {
        hw_set_error("Invalid UART handle %d", handle);
        return -1;
    }
    int n = (int)write(g_uarts[handle].fd, buf, len);
    if (n < 0) {
        hw_set_error("UART write error: %s", strerror(errno));
        return -1;
    }
    return n;
}

/* ===================== 8. 系统信息 ===================== */

/* 获取物理内存映射 (通过 sysctl) */
int hw_phys_mem_info(uint64_t* total, uint64_t* free_bytes) {
    if (total) {
        size_t len = sizeof(*total);
        sysctlbyname("hw.memsize", total, &len, NULL, 0);
    }
    if (free_bytes) {
        /* macOS 上用 vm_statistics 获取空闲内存 */
        mach_port_t host = mach_host_self();
        vm_size_t pagesize;
        host_page_size(host, &pagesize);

        vm_statistics64_data_t vmstat;
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        if (host_statistics64(host, HOST_VM_INFO64,
                              (host_info64_t)&vmstat, &count) == KERN_SUCCESS) {
            *free_bytes = (uint64_t)vmstat.free_count * pagesize;
        } else {
            *free_bytes = 0;
        }
    }
    return 0;
}