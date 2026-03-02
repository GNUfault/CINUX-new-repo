#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"

extern int exec(char*, char**);
extern void exit(int) __attribute__((noreturn));

int
main(void)
{
  char *argv[] = { "hello", 0 };

  exec("hello", argv);


  for(;;);
  return 0;
}