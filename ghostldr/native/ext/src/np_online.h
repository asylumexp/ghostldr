/*
 * Shared by the NP online families (hle_np_online*.c): WebApi, WebApi2,
 * Score, Tus/Tss, Matching2, Signaling, SessionSignaling, Party,
 * EntitlementAccess, SNS, UDS and GameIntent.
 *
 * Every library is virtual: the extension keeps its own contexts, requests
 * and handles and never calls SWRR's copy (its title identity, its callback
 * threads, its NpManager), except the few pass-throughs the spec allows.
 *
 * Persona (np-core's, the default): the user is signed up but signed out.
 * Local bookkeeping succeeds; anything that needs PSN fails at once with
 * the library's "not signed in" code, and async calls finish immediately.
 * Build switches (make -C ghostldr/native EXT_CFLAGS="-DNPO_PERSONA_A=1"):
 *   NPO_PERSONA_A  signed in but PSN unreachable: PSN failures become the
 *                  network error, Score/Tus/Tss title contexts succeed.
 *                  np-core's sceNpGetState must flip with it.
 *   NPO_M2_ASYNC   Matching2 ContextStart returns 0 and a pump thread
 *                  delivers START_OVER, for titles that ignore its result.
 */
#pragma once
#include "ext.h"

#ifndef NPO_PERSONA_A
#define NPO_PERSONA_A 0
#endif
#ifndef NPO_M2_ASYNC
#define NPO_M2_ASYNC 0
#endif

#define NPO_ERR_NET ((s32)0x8041013D)         /* SCE_NET_ERROR_ECONNREFUSED: what SWRR's own NP calls get offline */
#define NPO_NP_SIGNED_OUT ((s32)0x80550006)   /* SCE_NP_ERROR_SIGNED_OUT */

/* a library's "PSN is not there" answer: its signed-out code (persona B) or
 * the network error (persona A) */
#if NPO_PERSONA_A
#define NPO_OFF(signed_out, unreachable) ((s32)(unreachable))
#else
#define NPO_OFF(signed_out, unreachable) ((s32)(signed_out))
#endif

/* Logging: the first 4 calls of a call site, then up to 16 more failures.
 * Callers pass a static counter per site (NPO_TRACE). */
static __attribute__((noinline, unused)) bool npo_should_log(u32 *n, s32 ret)
{
    u32 c = __atomic_fetch_add(n, 1, __ATOMIC_RELAXED);
    return c < 4 || (ret < 0 && c < 20);
}

#define NPO_TRACE(ret, ...)                                                                                        \
    do {                                                                                                           \
        static u32 npo_n_;                                                                                         \
        if (npo_should_log(&npo_n_, (s32)(ret)))                                                                  \
            logf(__VA_ARGS__);                                                                                     \
    } while (0)

/* the first n calls of a call site */
#define NPO_LOG_FIRST(n, ...)                                                                                      \
    do {                                                                                                           \
        static u32 npo_n_;                                                                                         \
        if (__atomic_fetch_add(&npo_n_, 1, __ATOMIC_RELAXED) < (n))                                               \
            logf(__VA_ARGS__);                                                                                     \
    } while (0)

/* next id from a counter: 1..max, never 0, wrapping */
static inline s32 npo_next(s32 *counter, s32 max)
{
    if (*counter <= 0 || *counter >= max)
        *counter = 0;
    return ++*counter;
}

typedef u64 (*npo_fn6)(u64, u64, u64, u64, u64, u64);

/* These families are table-heavy and never hot: optimize their code for
 * size (no inlining of the shared helpers into every wrapper). */
#define NPO_SMALL_BEGIN _Pragma("clang attribute push(__attribute__((minsize)), apply_to = function)")
#define NPO_SMALL_END _Pragma("clang attribute pop")
