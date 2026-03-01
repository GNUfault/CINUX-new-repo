#ifndef SYSCALLS_H
#define SYSCALLS_H

int open(const char *path, int mode);
int mknod(const char *path, int major, int minor);
int dup(int fd);
int close(int fd);
int read(int fd, void *buf, int n);
int write(int fd, const void *buf, int n);
int fork(void);
int exec(const char *path, char *const argv[]);
int pipe(int fds[2]);
int kill(int pid);
int getpid(void);

#endif // SYSCALLS_H
