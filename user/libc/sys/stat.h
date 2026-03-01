#ifndef SYS_STAT_H
#define SYS_STAT_H

#include "types.h"

int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
int mknod(const char *path, mode_t mode, dev_t dev);

#endif
