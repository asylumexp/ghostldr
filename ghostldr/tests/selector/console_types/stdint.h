/* For cat_host (test_usb.py): the selector's integer types as on the
 * console, where uint64_t is unsigned long like ghost.h's u64 (on macOS it
 * is unsigned long long, and the two headers would disagree). */
#pragma once
typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long int64_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;
typedef long intptr_t;
typedef unsigned long uintptr_t;
