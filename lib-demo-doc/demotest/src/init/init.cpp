#include "init.h"
#include "tran.h"
#include "oam.h"
#include "cos.h"

#ifdef __cplusplus
extern "C" {
#endif

u32 InitGtpcore(const rolerEnum &en_roler) {
    u32 runing_result = COS_OK;

    runing_result = InitOam();
    if (COS_OK != runing_result) {
        return runing_result;
    }

    runing_result = InitTran(en_roler);
    if (COS_OK != runing_result) {
        return runing_result;
    }

    return COS_OK;
}

void RemoveGtpcore(void) {
    RemoveTran();
    RemoveOam();

    return;
}

#ifdef __cplusplus
}
#endif

