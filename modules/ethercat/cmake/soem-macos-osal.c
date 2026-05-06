/*
 * macOS osal for SOEM.
 *
 * Identical to osal/linux/osal.c except clock_nanosleep() is replaced:
 *   - osal_usleep: uses nanosleep() (relative, available everywhere)
 *   - osal_monotonic_sleep: computes remaining time vs CLOCK_MONOTONIC, nanosleeps
 *
 * ec_timet = struct timespec (see soem-macos-osal_defs.h).
 * Patched into SOEM source by EtherMouse's CMake configuration.
 */

#include <osal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

void osal_get_monotonic_time(ec_timet *ts)
{
   clock_gettime(CLOCK_MONOTONIC, ts);
}

ec_timet osal_current_time(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   return ts;
}

void osal_time_diff(ec_timet *start, ec_timet *end, ec_timet *diff)
{
   osal_timespecsub(end, start, diff);
}

void osal_timer_start(osal_timert *self, uint32 timeout_usec)
{
   struct timespec start_time;
   struct timespec timeout;

   osal_get_monotonic_time(&start_time);
   osal_timespec_from_usec(timeout_usec, &timeout);
   osal_timespecadd(&start_time, &timeout, &self->stop_time);
}

boolean osal_timer_is_expired(osal_timert *self)
{
   struct timespec current_time;
   int is_not_yet_expired;

   osal_get_monotonic_time(&current_time);
   is_not_yet_expired = osal_timespeccmp(&current_time, &self->stop_time, <);

   return is_not_yet_expired == FALSE;
}

int osal_usleep(uint32 usec)
{
   struct timespec ts;
   int result;

   osal_timespec_from_usec(usec, &ts);
   /* nanosleep: relative sleep, available on macOS */
   do {
      result = nanosleep(&ts, &ts);
   } while (result == -1 && errno == EINTR);
   return result == 0 ? 0 : -1;
}

int osal_monotonic_sleep(ec_timet *abs_ts)
{
   /* macOS lacks clock_nanosleep(TIMER_ABSTIME). Compute remaining time. */
   struct timespec now;
   struct timespec remaining;

   clock_gettime(CLOCK_MONOTONIC, &now);
   osal_timespecsub(abs_ts, &now, &remaining);

   if (remaining.tv_sec < 0 || (remaining.tv_sec == 0 && remaining.tv_nsec <= 0)) {
      return 0;  /* already past the target time */
   }

   int result;
   do {
      result = nanosleep(&remaining, &remaining);
   } while (result == -1 && errno == EINTR);
   return result == 0 ? 0 : -1;
}

void *osal_malloc(size_t size)
{
   return malloc(size);
}

void osal_free(void *ptr)
{
   free(ptr);
}

int osal_thread_create(void *thandle, int stacksize, void *func, void *param)
{
   int ret;
   pthread_attr_t attr;
   pthread_t *threadp = thandle;

   pthread_attr_init(&attr);
   pthread_attr_setstacksize(&attr, stacksize);
   ret = pthread_create(threadp, &attr, func, param);
   pthread_attr_destroy(&attr);
   return ret == 0 ? 1 : 0;
}

int osal_thread_create_rt(void *thandle, int stacksize, void *func, void *param)
{
   /* macOS: SCHED_FIFO requires entitlements; fall back to normal thread.
    * For actual RT, run on Linux with PREEMPT_RT. */
   return osal_thread_create(thandle, stacksize, func, param);
}

void *osal_mutex_create(void)
{
   pthread_mutexattr_t mutexattr;
   osal_mutext *mutex;

   mutex = (osal_mutext *)osal_malloc(sizeof(osal_mutext));
   if (mutex)
   {
      pthread_mutexattr_init(&mutexattr);
      pthread_mutexattr_setprotocol(&mutexattr, PTHREAD_PRIO_INHERIT);
      pthread_mutex_init(mutex, &mutexattr);
   }
   return (void *)mutex;
}

void osal_mutex_destroy(void *mutex)
{
   pthread_mutex_destroy((osal_mutext *)mutex);
   osal_free(mutex);
}

void osal_mutex_lock(void *mutex)
{
   pthread_mutex_lock((osal_mutext *)mutex);
}

void osal_mutex_unlock(void *mutex)
{
   pthread_mutex_unlock((osal_mutext *)mutex);
}
