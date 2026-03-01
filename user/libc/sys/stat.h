#ifndef SYS_STAT_H
#define SYS_STAT_H

#include <sys/types.h>

struct stat {
    dev_t dev;
    ino_t ino;
    short type;
    short nlink;
    off_t size;
};

int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
int mknod(const char *path, mode_t mode, dev_t dev);

#endif
