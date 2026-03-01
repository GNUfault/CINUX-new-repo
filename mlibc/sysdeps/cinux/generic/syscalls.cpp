#include <mlibc/all-sysdeps.hpp>
#include <errno.h>
#include <bits/ensure.h>

extern "C" {
#include <kernel/syscall.h> 
}

namespace mlibc {

#pragma GCC visibility push(default)

int sys_clock_get(int clock, long *sec, long *nsec) {
    *sec = 0;
    *nsec = 0;
    return 0; 
}

long __syscall(long num, long a0, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r7 asm("a7") = num;
    register long r0 asm("a0") = a0;
    register long r1 asm("a1") = a1;
    register long r2 asm("a2") = a2;
    register long r3 asm("a3") = a3;
    register long r4 asm("a4") = a4;
    register long r5 asm("a5") = a5;

    asm volatile ("ecall"
        : "+r"(r0) 
        : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
        : "memory");
    ret = r0;
    return ret;
}

int sys_anon_allocate(size_t size, void **pointer) {
    long ret = __syscall(SYS_sbrk, (long)size, 0, 0, 0, 0, 0);
    if (ret == -1) return ENOMEM;
    *pointer = (void*)ret;
    return 0;
}

int sys_anon_free(void *pointer, size_t size) {
    return 0;
}

int sys_write(int fd, const void *buf, size_t count, ssize_t *bytes_written) {
    long ret = __syscall(SYS_write, (long)fd, (long)buf, (long)count, 0, 0, 0);
    if (ret < 0) return EINVAL;
    *bytes_written = ret;
    return 0;
}

int sys_read(int fd, void *buf, size_t count, ssize_t *bytes_read) {
    long ret = __syscall(SYS_read, (long)fd, (long)buf, (long)count, 0, 0, 0);
    if (ret < 0) return EINVAL;
    *bytes_read = ret;
    return 0;
}

int sys_open(const char *path, int flags, mode_t mode, int *fd) {
    long ret = __syscall(SYS_open, (long)path, (long)flags, 0, 0, 0, 0);
    if (ret < 0) return ENOENT;
    *fd = (int)ret;
    return 0;
}

int sys_close(int fd) {
    long ret = __syscall(SYS_close, (long)fd, 0, 0, 0, 0, 0);
    return (ret < 0) ? EINVAL : 0;
}

int sys_seek(int fd, off_t offset, int whence, off_t *new_offset) { 
    return ENOSYS; 
}

int sys_vm_map(void *hint, size_t size, int prot, int flags, int fd, off_t offset, void **window) {
    return sys_anon_allocate(size, window);
}

int sys_futex_wait(int *pointer, int expected, const struct timespec *time) { return 0; }
int sys_futex_wake(int *pointer) { return 0; }

int sys_tcb_set(void *pointer) {
    asm volatile ("mv tp, %0" : : "r"(pointer));
    return 0;
}

[[noreturn]] void sys_exit(int status) {
    __syscall(SYS_exit, (long)status, 0, 0, 0, 0, 0);
    while(1);
}

void sys_libc_log(const char *message) {
    ssize_t unused;
    size_t len = 0;
    while(message[len]) len++;
    sys_write(2, message, len, &unused);
    sys_write(2, "\n", 1, &unused);
}

[[noreturn]] void sys_libc_panic() {
    sys_libc_log("!!! MLIBC PANIC !!!");
    sys_exit(1);
}

#pragma GCC visibility pop

}