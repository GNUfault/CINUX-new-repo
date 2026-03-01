#ifndef UNISTD_H
#define UNISTD_H

// TODO: add this
//#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

int fork(void);
int exit(int) __attribute__((noreturn));
int wait(int*);
int pipe(int*);
// TODO: add this
//int execve(const char *pathname, char *const argv[], char *const envp[]);
int write(int, const void*, int);
int read(int, void*, int);
int close(int);
int kill(int);
int open(const char*, int);
int unlink(const char*);
int link(const char*, const char*);
int mkdir(const char*);
int chdir(const char*);
int dup(int);
int getpid(void);
int pause(void);
void *sbrk(int increment);

#endif // UNISTD_H