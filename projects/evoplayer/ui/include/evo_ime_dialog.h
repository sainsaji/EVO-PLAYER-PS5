/*
 * evo_ime_dialog.h — Native PlayStation 5 On-Screen IME Keyboard Dialog.
 *
 * Exposes the native PS5 system IME dialog (libSceImeDialog / SCE_SYSMODULE_IME_DIALOG).
 * Provides multi-language input, word prediction, CJK support, and direct USB keyboard input.
 */
#ifndef EVO_IME_DIALOG_H
#define EVO_IME_DIALOG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SceImeType {
    SCE_IME_TYPE_DEFAULT      = 0,
    SCE_IME_TYPE_BASIC_LATIN  = 1,
    SCE_IME_TYPE_URL          = 2,
    SCE_IME_TYPE_MAIL         = 3,
    SCE_IME_TYPE_NUMBER       = 4
} SceImeType;

typedef enum SceImeEnterLabel {
    SCE_IME_ENTER_LABEL_DEFAULT = 0,
    SCE_IME_ENTER_LABEL_SEND    = 1,
    SCE_IME_ENTER_LABEL_SEARCH  = 2,
    SCE_IME_ENTER_LABEL_GO      = 3
} SceImeEnterLabel;

typedef enum SceImeInputMethod {
    SCE_IME_INPUT_METHOD_DEFAULT = 0
} SceImeInputMethod;

typedef enum SceImeHorizontalAlignment {
    SCE_IME_HALIGN_LEFT   = 0,
    SCE_IME_HALIGN_CENTER = 1,
    SCE_IME_HALIGN_RIGHT  = 2
} SceImeHorizontalAlignment;

typedef enum SceImeVerticalAlignment {
    SCE_IME_VALIGN_TOP    = 0,
    SCE_IME_VALIGN_CENTER = 1,
    SCE_IME_VALIGN_BOTTOM = 2
} SceImeVerticalAlignment;

typedef enum SceImeDialogStatus {
    SCE_IME_DIALOG_STATUS_NONE     = 0,
    SCE_IME_DIALOG_STATUS_RUNNING  = 1,
    SCE_IME_DIALOG_STATUS_FINISHED = 2
} SceImeDialogStatus;

typedef enum SceImeDialogEndStatus {
    SCE_IME_DIALOG_END_STATUS_OK            = 0,
    SCE_IME_DIALOG_END_STATUS_USER_CANCELED = 1,
    SCE_IME_DIALOG_END_STATUS_ABORTED       = 2
} SceImeDialogEndStatus;

typedef enum SceImeOption {
    SCE_IME_OPTION_NONE                   = 0x00000000,
    SCE_IME_OPTION_MULTILINE              = 0x00000001,
    SCE_IME_OPTION_NO_AUTO_CAPITALIZATION = 0x00000002,
    SCE_IME_OPTION_PASSWORD               = 0x00000004,
    SCE_IME_OPTION_EXTERNAL_KEYBOARD      = 0x00000010,
    SCE_IME_OPTION_NO_LEARNING            = 0x00000020,
    SCE_IME_OPTION_FIXED_POSITION         = 0x00000040,
    SCE_IME_OPTION_DISABLE_COPY_PASTE     = 0x00000080
} SceImeOption;

typedef struct SceImeDialogParam {
    int32_t                 userId;
    uint32_t                type;               /* SceImeType */
    uint64_t                supportedLanguages;
    int32_t                 enterLabel;         /* SceImeEnterLabel */
    int32_t                 inputMethod;        /* SceImeInputMethod */
    void                   *filter;
    uint32_t                option;             /* SceImeOption */
    uint32_t                maxTextLength;
    uint16_t               *inputTextBuffer;    /* UTF-16 pointer */
    float                   posx;
    float                   posy;
    int32_t                 horizontalAlignment;/* SceImeHorizontalAlignment */
    int32_t                 verticalAlignment;  /* SceImeVerticalAlignment */
    const uint16_t         *placeholder;        /* UTF-16 pointer */
    const uint16_t         *title;              /* UTF-16 pointer */
    int8_t                  reserved[16];
} SceImeDialogParam;

typedef struct SceImeDialogResult {
    int32_t                 endStatus;          /* SceImeDialogEndStatus */
    int8_t                  reserved[12];
} SceImeDialogResult;

#define SCE_SYSMODULE_IME_DIALOG 0x0096

#if defined(EVO_TARGET_PS5) || defined(__FreeBSD__)
/* Native PS5 symbols from libSceImeDialog and libSceUserService */
int sceImeDialogInit(const SceImeDialogParam *param, void *extendedParam);
int sceImeDialogGetStatus(void);
int sceImeDialogGetResult(SceImeDialogResult *result);
int sceImeDialogAbort(void);
int sceImeDialogTerm(void);
int sceImeDialogGetPanelSizeExtended(const SceImeDialogParam *param, void *extendedParam, uint32_t *width, uint32_t *height);

int sceUserServiceGetInitialUser(int *userId);
int sceUserServiceGetLoginUserIdList(int *userIdList);

int sceKernelLoadStartModule(const char *name, size_t argc, const void *argv, unsigned int flags, void *opt, int *res);
int sceKernelDlsym(int moduleHandle, const char *symbol, void **addrOut);
#endif


#ifdef __cplusplus
}
#endif


#endif /* EVO_IME_DIALOG_H */
