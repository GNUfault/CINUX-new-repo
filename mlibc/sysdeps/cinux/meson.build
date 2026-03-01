# Syscalls are needed by both the dynamic linker and the main libc
rtld_sources += files('generic/syscalls.cpp')
libc_sources += files('generic/syscalls.cpp')

# entry.cpp contains _start, which belongs in libc.so (or crt0.o)
libc_sources += files('generic/entry.cpp')

sysdep_supported_options = {
    'posix': true,
    'linux': false,
    'glibc': true,
    'bsd': false,
}

# The 'sources' variable is often used for static sub-libraries; 
# we'll keep it consistent with the above.
sources = files(
    'generic/syscalls.cpp',
    'generic/entry.cpp',
)

install_headers(
    'include/abi-bits/access.h',
    'include/abi-bits/errno.h',
    'include/abi-bits/fcntl.h',
    'include/abi-bits/stat.h',
    'include/abi-bits/signal.h',
    'include/abi-bits/socket.h',
    'include/abi-bits/termios.h',
    'include/abi-bits/time.h',
    'include/abi-bits/resource.h',
    'include/abi-bits/wait.h',
    'include/abi-bits/poll.h',
    'include/abi-bits/utsname.h',
    subdir: 'abi-bits'
)