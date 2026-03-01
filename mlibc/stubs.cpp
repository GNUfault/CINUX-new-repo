#include <stddef.h>

extern "C" {
    void __dlapi_close() { }
    void __dlapi_error() { }
    void __dlapi_open() { }
    void __dlapi_resolve() { }
    void __dlapi_reverse() { }
    void __dlapi_find_object() { }
    void __dlapi_get_config() { }

    void *__dlapi_entrystack = nullptr;
    void *__rtld_allocateTcb() { return nullptr; }
    void *__dlapi_get_tls(void *) { return nullptr; }
    void __dlapi_exit() { while(1); }
    int __dlapi_iterate_phdr(int (*)(void*, size_t, void*), void*) { return 0; }
}