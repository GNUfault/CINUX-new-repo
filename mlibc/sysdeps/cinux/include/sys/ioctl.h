#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#include <abi-bits/ioctls.h>

extern "C" {
    int ioctl(int fd, unsigned long request, ...);
}

#endif