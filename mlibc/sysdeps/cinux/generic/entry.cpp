#include <mlibc/all-sysdeps.hpp>

namespace mlibc {
    void entry_stack_dtm(void (*ptr)()) {
    }
}

extern "C" void __dlapi_entrystack(); 

extern "C" void _start() {
    mlibc::entry_stack_dtm(reinterpret_cast<void(*)()>(__dlapi_entrystack));
}