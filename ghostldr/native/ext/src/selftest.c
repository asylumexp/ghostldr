/* Self-test the core runs right after ext_init (ghost_api.h): a pointer in
 * relocated data, a bss counter and a call back into the core, reached by
 * name, by NID alone and through a library alias.  It lives in a library no
 * title imports. */
#include "ext.h"

static const char *volatile g_word = EXT_SELFTEST_NAME;
static volatile u32 g_calls;

static u64 ext_selftest(u64 seed)
{
    u32 n = ++g_calls;
    if (strcmp(g_word, "ghostldrExtSelfTest") != 0)
        return 0;
    if (n == 1)
        logf("ext: self-test running at %p", (void *)ext_selftest);
    return EXT_SELFTEST_OK ^ seed ^ ((u64)n << 32);
}

static const struct ext_hle_ent k_hle[] = {
    {EXT_SELFTEST_LIB, EXT_SELFTEST_NAME, ext_selftest, NULL},
    {EXT_SELFTEST_LIB, NULL, ext_selftest, EXT_SELFTEST_NID},
};

static const struct ext_lib k_libs[] = {
    {EXT_SELFTEST_LIB, EXT_SELFTEST_ALIAS, EXT_LIB_NO_HOST, EXT_SELFTEST_STUB_RET},
};

EXT_FAMILY_LIBS("selftest", k_hle, k_libs, NULL);
