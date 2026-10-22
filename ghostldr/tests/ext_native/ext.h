/*
 * ext.h for extension families built for this machine (test_homebrew.py):
 * the real header, with the family macros registering each family with the
 * harness at start-up instead of placing it in the ext_families section.
 */
#include_next "ext.h"

void harness_family(const struct ext_family *f);

#undef EXT_FAMILY_HOOKS
#define EXT_FAMILY_HOOKS(family, table, lib_table, init_fn, sysmodule_fn)                                        \
    static const struct ext_family ext_family_ = {family, table, ARRAY_SIZE(table), ARRAY_SIZE(lib_table),        \
                                                  lib_table, init_fn, sysmodule_fn};                              \
    __attribute__((constructor)) static void ext_family_register_(void) { harness_family(&ext_family_); }

#undef EXT_FAMILY
#define EXT_FAMILY(family, table, init_fn)                                                                        \
    static const struct ext_family ext_family_ = {family, table, ARRAY_SIZE(table), 0, NULL, init_fn, NULL};      \
    __attribute__((constructor)) static void ext_family_register_(void) { harness_family(&ext_family_); }
