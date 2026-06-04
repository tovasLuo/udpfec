#ifndef _OAM_H_
#define _OAM_H_

#include "cos.h"

#ifdef __cplusplus
extern "C" {
#endif

u8* GetAppVersion(u8 *pOutVer);

u32 InitOam(void);

void RemoveOam(void);

#ifdef _UTTEST
u32 CmdShowC3coreVer(void);
#endif

#ifdef __cplusplus
}
#endif

#endif

