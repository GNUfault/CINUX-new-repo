#ifndef _UTMPX_H
#define _UTMPX_H

#include <abi-bits/utmpx.h>

#ifdef __cplusplus
extern "C" {
#endif

struct utmpx *getutxent(void);
void setutxent(void);
void endutxent(void);
struct utmpx *pututxline(const struct utmpx *);

#ifdef __cplusplus
}
#endif

#endif