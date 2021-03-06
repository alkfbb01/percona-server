/*****************************************************************************

Copyright (c) 2013, 2018, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
limited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
included with MySQL.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file os/os0event.cc
 The interface to the operating system condition variables.

 Created 2012-09-23 Sunny Bains
 *******************************************************/

#include "os0event.h"

#include <errno.h>
#include <time.h>

#include "ha_prototypes.h"
#include "ut0mutex.h"
#include "ut0new.h"

#ifdef _WIN32
#include <windows.h>
#endif /* _WIN32 */

#include <list>

/** The number of microsecnds in a second. */
static const ulint MICROSECS_IN_A_SECOND = 1000000;

bool os_event::timed_wait(
#ifndef _WIN32
    const timespec *abstime
#else
    DWORD time_in_ms
#endif /* !_WIN32 */
) {
#ifdef _WIN32
  BOOL ret;

  ret = SleepConditionVariableCS(&cond_var, mutex, time_in_ms);

  if (!ret) {
    DWORD err = GetLastError();

    /* FQDN=msdn.microsoft.com
    @see http://$FQDN/en-us/library/ms686301%28VS.85%29.aspx,

    "Condition variables are subject to spurious wakeups
    (those not associated with an explicit wake) and stolen wakeups
    (another thread manages to run before the woken thread)."
    Check for both types of timeouts.
    Conditions are checked by the caller.*/
    if (err == WAIT_TIMEOUT || err == ERROR_TIMEOUT) {
      return (true);
    }
  }

  ut_a(ret);

  return (false);
#else
  int ret;

  ret = pthread_cond_timedwait(&cond_var, mutex, abstime);

  switch (ret) {
    case 0:
    case ETIMEDOUT:
      /* We play it safe by checking for EINTR even though
      according to the POSIX documentation it can't return EINTR. */
    case EINTR:
      break;

    default:
#ifdef UNIV_NO_ERR_MSGS
      ib::error()
#else
      ib::error(ER_IB_MSG_742)
#endif /* !UNIV_NO_ERR_MSGS */
          << "pthread_cond_timedwait() returned: " << ret << ": abstime={"
          << abstime->tv_sec << "," << abstime->tv_nsec << "}";
      ut_error;
  }

  return (ret == ETIMEDOUT);
#endif /* _WIN32 */
}

/**
Waits for an event object until it is in the signaled state.

Typically, if the event has been signalled after the os_event_reset()
we'll return immediately because event->m_set == true.
There are, however, situations (e.g.: sync_array code) where we may
lose this information. For example:

thread A calls os_event_reset()
thread B calls os_event_set()   [event->m_set == true]
thread C calls os_event_reset() [event->m_set == false]
thread A calls os_event_wait()  [infinite wait!]
thread C calls os_event_wait()  [infinite wait!]

Where such a scenario is possible, to avoid infinite wait, the
value returned by reset() should be passed in as
reset_sig_count. */
void os_event::wait_low(int64_t reset_sig_count) UNIV_NOTHROW {
  mutex.enter();

  if (!reset_sig_count) {
    reset_sig_count = signal_count();
  }

  while (!is_set() && signal_count() == reset_sig_count) {
    wait();

    /* Spurious wakeups may occur: we have to check if the
    event really has been signaled after we came here to wait. */
  }

  mutex.exit();
}

/**
Waits for an event object until it is in the signaled state or
a timeout is exceeded.
@param time_in_usec - timeout in microseconds, or OS_SYNC_INFINITE_TIME
@param reset_sig_count - zero or the value returned by previous call
        of os_event_reset().
@return	0 if success, OS_SYNC_TIME_EXCEEDED if timeout was exceeded */
ulint os_event::wait_time_low(ulint time_in_usec,
                              int64_t reset_sig_count) UNIV_NOTHROW {
  bool timed_out = false;

#ifdef _WIN32
  DWORD time_in_ms;

  if (time_in_usec != OS_SYNC_INFINITE_TIME) {
    time_in_ms = DWORD(time_in_usec / 1000);
  } else {
    time_in_ms = INFINITE;
  }
#else
  struct timespec abstime;

  if (time_in_usec != OS_SYNC_INFINITE_TIME) {
    struct timeval tv;
    int ret;
    ulint sec;
    ulint usec;

    ret = ut_usectime(&sec, &usec);
    ut_a(ret == 0);

    tv.tv_sec = sec;
    tv.tv_usec = usec;

    tv.tv_usec += time_in_usec;

    if ((ulint)tv.tv_usec >= MICROSECS_IN_A_SECOND) {
      tv.tv_sec += tv.tv_usec / MICROSECS_IN_A_SECOND;
      tv.tv_usec %= MICROSECS_IN_A_SECOND;
    }

    abstime.tv_sec = tv.tv_sec;
    abstime.tv_nsec = tv.tv_usec * 1000;
  } else {
    abstime.tv_nsec = 999999999;
    abstime.tv_sec = std::numeric_limits<time_t>::max();
  }

  ut_a(abstime.tv_nsec <= 999999999);

#endif /* _WIN32 */

  mutex.enter();

  if (!reset_sig_count) {
    reset_sig_count = signal_count();
  }

  do {
    if (is_set() || signal_count() != reset_sig_count) {
      break;
    }

#ifndef _WIN32
    timed_out = timed_wait(&abstime);
#else
    timed_out = timed_wait(time_in_ms);
#endif /* !_WIN32 */

  } while (!timed_out);

  mutex.exit();

  return (timed_out ? OS_SYNC_TIME_EXCEEDED : 0);
}

/** Constructor */
os_event::os_event(void) UNIV_NOTHROW {
  init();

  /* We return this value in os_event_reset(),
  which can then be be used to pass to the
  os_event_wait_low(). The value of zero is
  reserved in os_event_wait_low() for the case
  when the caller does not want to pass any
  signal_count value. To distinguish between
  the two cases we initialize signal_count
  to 1 here. */

  count_and_set = 1;
}

/** Destructor */
os_event::~os_event() UNIV_NOTHROW { destroy(); }

/**
Creates an event semaphore, i.e., a semaphore which may just have two
states: signaled and nonsignaled. The created event is manual reset: it
must be reset explicitly by calling sync_os_reset_event.
@return	the event handle */
os_event_t os_event_create(
    const char *name UNIV_UNUSED) /*!< in: the name of the
                      event, if NULL the event
                      is created without a name */
{
  os_event_t ret = (UT_NEW_NOKEY(os_event()));
/**
 On SuSE Linux we get spurious EBUSY from pthread_mutex_destroy()
 unless we grab and release the mutex here. Current OS version:
 openSUSE Leap 15.0
 Linux xxx 4.12.14-lp150.12.25-default #1 SMP
 Thu Nov 1 06:14:23 UTC 2018 (3fcf457) x86_64 x86_64 x86_64 GNU/Linux */
#if defined(LINUX_SUSE)
  os_event_reset(ret);
#endif
  return ret;
}

/**
Check if the event is set.
@return true if set */
bool os_event_is_set(const os_event_t event) /*!< in: event to test */
{
  return (event->is_set());
}

/**
Sets an event semaphore to the signaled state: lets waiting threads
proceed. */
void os_event_set(os_event_t event) /*!< in/out: event to set */
{
  event->set();
}

bool os_event_try_set(os_event_t event) { return (event->try_set()); }

/**
Resets an event semaphore to the nonsignaled state. Waiting threads will
stop to wait for the event.
The return value should be passed to os_even_wait_low() if it is desired
that this thread should not wait in case of an intervening call to
os_event_set() between this os_event_reset() and the
os_event_wait_low() call. See comments for os_event_wait_low().
@return	current signal_count. */
int64_t os_event_reset(os_event_t event) /*!< in/out: event to reset */
{
  return (event->reset());
}

/**
Waits for an event object until it is in the signaled state or
a timeout is exceeded.
@return	0 if success, OS_SYNC_TIME_EXCEEDED if timeout was exceeded */
ulint os_event_wait_time_low(
    os_event_t event,        /*!< in/out: event to wait */
    ulint time_in_usec,      /*!< in: timeout in
                             microseconds, or
                             OS_SYNC_INFINITE_TIME */
    int64_t reset_sig_count) /*!< in: zero or the value
                             returned by previous call of
                             os_event_reset(). */
{
  return (event->wait_time_low(time_in_usec, reset_sig_count));
}

/**
Waits for an event object until it is in the signaled state.

Where such a scenario is possible, to avoid infinite wait, the
value returned by os_event_reset() should be passed in as
reset_sig_count. */
void os_event_wait_low(os_event_t event,        /*!< in: event to wait */
                       int64_t reset_sig_count) /*!< in: zero or the value
                                                returned by previous call of
                                                os_event_reset(). */
{
  event->wait_low(reset_sig_count);
}

/**
Frees an event object. */
void os_event_destroy(os_event_t &event) /*!< in/own: event to free */

{
  if (event != NULL) {
    UT_DELETE(event);
    event = NULL;
  }
}
