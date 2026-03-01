#include <stddef.h>

extern "C" {
    void *__dlapi_entrystack = nullptr;

    void *__rtld_allocateTcb() { return nullptr; }

    void *__dlapi_get_tls(void *) { return nullptr; }

    void __dlapi_exit() { while(1); }

    int __dlapi_iterate_phdr(int (*)(void*, size_t, void*), void*) { return 0; }
}