/*
 * dsp_prelude.h - force-included (-include) into every DfxDsp translation unit
 * on Linux.
 *
 * WHY: The DSP engine was written for Windows, where `long` is 32 bits (LLP64).
 * It relies on that everywhere: parameter blocks are `long` arrays that hold
 * floats bit-for-bit, DSP memory images are addressed in `long` words, and so
 * on. On 64-bit Linux (LP64) `long` is 64 bits, which corrupts all of that
 * (misaligned reads, stack overflows, NaN output).
 *
 * HOW: First include every system header the DSP uses, so the C/C++ runtime
 * is declared with its real types. Then map the keyword `long` to `int`, so
 * the DSP's own code sees the same 32-bit `long` it has on Windows. Include
 * guards stop the system headers being re-read later under the macro.
 * `linux/check_dsp_prelude.sh` verifies that no system header is first parsed
 * after the macro is defined.
 *
 * Constructs the macro can't handle ("long long", "long int", "long double")
 * turn into "int int" etc. and fail to COMPILE, so problems are loud, never
 * silent.
 */
#ifndef FX_DSP_PRELUDE_H
#define FX_DSP_PRELUDE_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#include <malloc.h>
#include <memory.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef __cplusplus
#include <cstdint>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cwchar>
#include <ctime>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <algorithm>
#include <new>
#endif

#include "windows.h"   /* the Win32 shim in this directory */

/* Everything below this line is DSP code: give it Windows' 32-bit long. */
#define long int

/* The DSP's integer printf/scanf conversions ("%ld", "%lu") now refer to
 * 32-bit values; the format wrappers in win_compat.cpp honor this. */
#define FX_LONG_IS_32BIT 1

#endif
