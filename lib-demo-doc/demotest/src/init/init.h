#ifndef _INIT_H_
#define _INIT_H_

#include "macrodefine.h"
#include "cos.h"

#ifdef __cplusplus
extern "C" {
#endif

u32 InitGtpcore(const rolerEnum &en_roler);

void RemoveGtpcore(void);

#ifdef __cplusplus
}
#endif

#endif

