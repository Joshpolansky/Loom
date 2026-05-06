/*
 * Fixed osal_defs.h for macOS — the contrib version is missing ec_timet
 * and osal_mutext which osal/osal.h depends on.
 * Patched into soem source by EtherMouse's CMake configuration.
 */

#ifndef _osal_defs_
#define _osal_defs_

#ifdef __cplusplus
extern "C" {
#endif

#include <pthread.h>

#ifdef EC_DEBUG
#  include <stdio.h>
#  define EC_PRINT printf
#else
#  define EC_PRINT(...) do {} while (0)
#endif

#ifndef OSAL_PACKED
#  define OSAL_PACKED_BEGIN
#  define OSAL_PACKED __attribute__((__packed__))
#  define OSAL_PACKED_END
#endif

/*
 * ec_timet = struct timespec: macros in osal/osal.h (osal_timespecadd etc.)
 * and core SOEM (ec_dc.c) access .tv_sec / .tv_nsec directly.
 */
#include <time.h>
#define ec_timet            struct timespec

#define OSAL_THREAD_HANDLE  pthread_t *
#define OSAL_THREAD_FUNC    void
#define OSAL_THREAD_FUNC_RT void

#define osal_mutext         pthread_mutex_t

#ifdef __cplusplus
}
#endif

#endif /* _osal_defs_ */
