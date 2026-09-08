/* linux/if_tun.h — macOS stub for TinyEMU build.
 * TAP networking (TUNSETIFF) is a Linux-only feature we don't need.
 * macOS already defines struct ifreq in <net/if.h>; we only add the
 * missing Linux tun constants. The "tap" driver is never instantiated
 * in our embedded headless config, so TUNSETIFF is never actually called.
 */
#ifndef LINUX_IF_TUN_H
#define LINUX_IF_TUN_H

#include <net/if.h>
#include <sys/ioctl.h>

#define IFF_TAP   0x0002
#define IFF_NO_PI 0x1000
#define TUNSETIFF 0x400454ca

#endif /* LINUX_IF_TUN_H */
