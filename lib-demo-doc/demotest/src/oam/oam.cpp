#include "oam.h"
#include "macrodefine.h"
#include "goodtp.h"
#include "cos.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

u8* GetAppVersion(u8 *pOutVer) {
    char version_type[64] = {0};

    cos_assertb((NULL == pOutVer), NULL);

    #ifdef _SELFDEBUG
    snprintf(&(version_type[0]), sizeof(version_type), "_debug");
    #else
    snprintf(&(version_type[0]), sizeof(version_type), "_release");
    #endif

    #ifdef _WIN32
    #pragma warning(disable:4996)
    sprintf((char*)pOutVer, "%s_%s  %s%s", // NOLINT
            __DATE__, __TIME__, _PBUILDVERSION, &(version_type[0]));
    #pragma warning(default:)
    #endif

    #ifdef __linux__
    sprintf((char*)pOutVer, "%s_%s  %s%s", // NOLINT
            __DATE__, __TIME__, PBUILDVERSION, &(version_type[0]));
    #endif

    return pOutVer;
}

u32 CmdShowC3coreVer(void) {
    u8 version[256] = {0};

    GetAppVersion(&(version[0]));

    cos_printf(" tester application version : %s\r\n", (char*)version);

    memset(version, 0x00, 256);
    GetGtpVersion(&(version[0]));
    cos_printf("              goodtp version : %s\r\n", (char*)version);

    return COS_OK;
}

void RegisterOamCmd(void) {
    u32 com_variable = 0;
    u32 length       = 0;

    cos_symbol_item_stru symbItem;

    memset(&symbItem, 0x00, sizeof(symbItem));

    symbItem.enSymbolAuth = cos_symbol_auth_normal;
    symbItem.enSymbolType = cos_symbol_type_function;
    symbItem.symbolAddr   = (void*)CmdShowC3coreVer; // NOLINT

    strcpy((char*)(symbItem.symbolName), "pAppVersion"); // NOLINT

    sprintf((char*)(symbItem.helpInfo), // NOLINT
            "The command execution format is pAppVersion [param]. It shows the ncu application version.\r\n");

    com_variable = (u32)strlen((char*)(symbItem.helpInfo)) + 1; // NOLINT

    sprintf((char*)(&(symbItem.helpInfo[com_variable])), "\r\n The param is optional, as follows:\r\n"); // NOLINT
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable]))); // NOLINT

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),  // NOLINT
            "  -h/?:\r\n\tIt shows this command help information.\r\n");

    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable]))); // NOLINT

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])), // NOLINT
            "  -others or no:\r\n\tIt shows the ncu application version.\r\n\n");

    cos_registerCodeSymbol(&symbItem);

    return;
}

u32 InitOam(void) {
    RegisterOamCmd();

    return COS_OK;
}

void RemoveOam(void) {
    return;
}

#ifdef __cplusplus
}
#endif

