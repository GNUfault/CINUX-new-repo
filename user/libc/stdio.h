#ifndef STDIO_H
#define STDIO_H

#include "sys/types.h"

typedef struct FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int printf(const char*, ...);
int fprintf(FILE *stream, const char *fmt, ...);
char *gets(char *s);

#endif // STDIO_H