sysdep_supported_options = ['posix', 'linux']

if not headers_only
    sources += files(
        'generic/syscalls.cpp',
        'generic/entry.cpp',
    )
endif

install_headers(
    'include/abi-bits/access.h',
    'include/abi-bits/errno.h',
    'include/abi-bits/fcntl.h',
    'include/abi-bits/stat.h',
    'include/abi-bits/signal.h',
    'include/abi-bits/reent.h',
    'include/abi-bits/socket.h',
    'include/abi-bits/termios.h',
    'include/abi-bits/time.h',
    'include/abi-bits/resource.h',
    'include/abi-bits/wait.h',
    'include/abi-bits/poll.h',
    'include/abi-bits/mman.h',
    'include/abi-bits/utsname.h',
    subdir: 'abi-bits'
)