#ifndef STRING_H
#define STRING_H

#include <sys/types.h>

char* strcpy(char*, const char*);
char* strchr(const char*, char);
int strcmp(const char*, const char*);
size_t strlen(const char*);
void* memset(void*, int, uint);
int memcmp(const void*, const void*, uint);
void* memcpy(void*, const void*, uint);
void* memmove(void*, const void*, int);

#endif // STRING_H