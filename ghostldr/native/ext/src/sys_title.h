/* The loaded title's identity, shared by the system families.  It lives in
 * hle_appcontent.c: the title's sce_sys/param.sfo, else a per-title fallback
 * table (dumps without param.sfo, such as Ghost's).  Inside SWRR every real
 * identity query answers for SWRR (CUSA03474), so these answer instead. */
#pragma once
#include "ext.h"

/* "CUSA11456": param.sfo's TITLE_ID, else the ghostfs index's; never NULL */
const char *title_ident(void);
/* a string key of param.sfo ("CONTENT_ID", "TITLE", "TITLE_01", ...), NULL if absent */
const char *title_param_str(const char *key);
/* an integer key ("USER_DEFINED_PARAM_1", "DOWNLOAD_DATA_SIZE", ...): false if absent */
bool title_param_int(const char *key, s32 *out);
/* the system language (sceSystemServiceParamGetInt LANG), -1 if unknown */
s32 system_lang(void);
