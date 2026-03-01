#ifndef STDIO_H
#define STDIO_H

#include <sys/types.h>

int printf(const char*, ...);
int fprintf(FILE *stream, const char *fmt, ...);
int gets(char*, int);

#endif // STDIO_H