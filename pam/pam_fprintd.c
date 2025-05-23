/*
 * pam_fprint: PAM module for fingerprint authentication through fprintd
 * Copyright (C) 2007 Daniel Drake <dsd@gentoo.org>
 * Copyright (C) 2008-2014, 2017-2020 Bastien Nocera <hadess@hadess.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <config.h>
#include <security/_pam_types.h>

#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <syslog.h>
#include <errno.h>

#include <pthread.h>

#include <libintl.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <poll.h>
#include <termios.h>

#define PAM_SM_AUTH
#include <security/pam_modules.h>
#include <security/pam_ext.h>

#define _(s) ((char *)dgettext(GETTEXT_PACKAGE, s))
#define TR(s) dgettext(GETTEXT_PACKAGE, s)
#define N_(s) (s)

#include "fingerprint-strings.h"
#include "pam_fprintd_autoptrs.h"

#define DEFAULT_MAX_TRIES 3
#define DEFAULT_TIMEOUT 30
#define MIN_TIMEOUT 10

#define DEBUG_MATCH "debug="
#define MAX_TRIES_MATCH "max-tries="
#define FP_MAX_TRIES_SWITCH_TO_PW "fp-max-tries-switch-to-pw"
#define TIMEOUT_MATCH "timeout="
#define NO_NEED_ENTER_MATCH "no-need-enter"
#define NO_PTHREAD_MATCH "no-pthread"
#define NO_PTHREAD_PW_FIRST_MATCH "no-pthread=pw-first"

static bool debug = false;
static unsigned max_tries = DEFAULT_MAX_TRIES;
static unsigned timeout = DEFAULT_TIMEOUT;
static bool no_need_enter = false;
static bool no_pthread = false;
static bool pw_first = false;
static bool max_tries_switch_to_pw = false;
#define USEC_PER_SEC ((uint64_t)1000000ULL)
#define NSEC_PER_USEC ((uint64_t)1000ULL)
#define USEC_PER_MSEC ((uint64_t)1000ULL)

static size_t user_enrolled_prints_num(pam_handle_t *pamh,
                                       sd_bus *bus,
                                       const char *dev,
                                       const char *username);

static uint64_t
now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * USEC_PER_SEC + (uint64_t)ts.tv_nsec / NSEC_PER_USEC;
}

static bool
str_has_prefix(const char *s, const char *prefix)
{
  if (s == NULL || prefix == NULL)
    return false;
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool
send_msg(pam_handle_t *pamh, const char *msg, int style)
{
  const struct pam_message mymsg = {
      .msg_style = style,
      .msg = msg,
  };
  const struct pam_message *msgp = &mymsg;
  const struct pam_conv *pc;
  struct pam_response *resp;

  if (pam_get_item(pamh, PAM_CONV, (const void **)&pc) != PAM_SUCCESS)
    return false;

  if (!pc || !pc->conv)
    return false;

  return pc->conv(1, &msgp, &resp, pc->appdata_ptr) == PAM_SUCCESS;
}

static bool
send_info_msg(pam_handle_t *pamh, const char *msg)
{
  return send_msg(pamh, msg, PAM_TEXT_INFO);
}

static bool
send_err_msg(pam_handle_t *pamh, const char *msg)
{
  return send_msg(pamh, msg, PAM_ERROR_MSG);
}

static char *
open_device(pam_handle_t *pamh,
            sd_bus *bus,
            const char *username,
            bool *has_multiple_devices)
{
  pf_auto(sd_bus_error) error = SD_BUS_ERROR_NULL;
  pf_autoptr(sd_bus_message) m = NULL;
  size_t num_devices;
  size_t max_prints;
  const char *path = NULL;
  const char *s;
  int r;

  *has_multiple_devices = false;

  if (sd_bus_call_method(bus,
                         "net.reactivated.Fprint",
                         "/net/reactivated/Fprint/Manager",
                         "net.reactivated.Fprint.Manager",
                         "GetDevices",
                         &error,
                         &m,
                         NULL) < 0)
  {
    pam_syslog(pamh, LOG_ERR, "GetDevices failed: %s", error.message);
    return NULL;
  }

  r = sd_bus_message_enter_container(m, 'a', "o");
  if (r < 0)
  {
    pam_syslog(pamh, LOG_ERR, "Failed to parse answer from GetDevices(): %d", r);
    return NULL;
  }

  num_devices = 0;
  max_prints = 0;
  while (sd_bus_message_read_basic(m, 'o', &s) > 0)
  {
    size_t enrolled_prints = user_enrolled_prints_num(pamh, bus, s, username);

    if (debug)
      pam_syslog(pamh, LOG_DEBUG, "%s prints registered: %" PRIu64, s, enrolled_prints);

    if (enrolled_prints > max_prints)
    {
      max_prints = enrolled_prints;
      path = s;
    }

    num_devices++;
  }
  *has_multiple_devices = (num_devices > 1);
  if (debug)
    pam_syslog(pamh, LOG_DEBUG, "Using device %s (out of %ld devices)", path, num_devices);

  sd_bus_message_exit_container(m);

  return path ? strdup(path) : NULL;
}

typedef struct
{
  char *dev;
  bool has_multiple_devices;

  unsigned max_tries;
  char *result;
  bool timed_out;
  bool is_swipe;
  bool verify_started;
  int verify_ret;
  pam_handle_t *pamh;

  char *driver;

  bool stop_got_pw;
  int pam_prompt_result;
  pid_t ppid;
  bool fingerprint_enabled; // Flag to indicate if fingerprint auth is available
} verify_data;

static void
verify_data_free(verify_data *data)
{
  free(data->result);
  free(data->driver);
  free(data->dev);
  free(data);
}

PF_DEFINE_AUTOPTR_CLEANUP_FUNC(verify_data, verify_data_free)

static int
verify_result(sd_bus_message *m,
              void *userdata,
              sd_bus_error *ret_error)
{
  verify_data *data = userdata;
  const char *msg;
  const char *result = NULL;
  /* see https://github.com/systemd/systemd/issues/14643 */
  uint64_t done = false;
  int r;

  if (!sd_bus_message_is_signal(m, "net.reactivated.Fprint.Device", "VerifyStatus"))
  {
    pam_syslog(data->pamh, LOG_ERR, "Not the signal we expected (iface: %s, member: %s)",
               sd_bus_message_get_interface(m),
               sd_bus_message_get_member(m));
    return 0;
  }

  if ((r = sd_bus_message_read(m, "sb", &result, &done)) < 0)
  {
    pam_syslog(data->pamh, LOG_ERR, "Failed to parse VerifyResult signal: %d", r);
    data->verify_ret = PAM_AUTHINFO_UNAVAIL;
    return 0;
  }

  if (!data->verify_started)
  {
    pam_syslog(data->pamh, LOG_ERR, "Unexpected VerifyResult '%s', %" PRIu64 " signal", result, done);
    return 0;
  }

  if (debug)
    pam_syslog(data->pamh, LOG_DEBUG, "Verify result: %s (done: %d)", result, done ? 1 : 0);

  if (data->result)
  {
    free(data->result);
    data->result = NULL;
  }

  if (done && result)
  {
    data->result = strdup(result);
    return 0;
  }

  msg = verify_result_str_to_msg(result, data->is_swipe);
  if (!msg)
  {
    data->result = strdup("Protocol error with fprintd!");
    return 0;
  }
  send_err_msg(data->pamh, msg);

  return 0;
}

static int
verify_finger_selected(sd_bus_message *m,
                       void *userdata,
                       sd_bus_error *ret_error)
{
  verify_data *data = userdata;
  const char *finger_name = NULL;
  pf_autofree char *msg = NULL;

  if (sd_bus_message_read_basic(m, 's', &finger_name) < 0)
  {
    pam_syslog(data->pamh, LOG_ERR, "Failed to parse VerifyFingerSelected signal: %d", errno);
    data->verify_ret = PAM_AUTHINFO_UNAVAIL;
    return 0;
  }

  if (!data->verify_started)
  {
    pam_syslog(data->pamh, LOG_ERR, "Unexpected VerifyFingerSelected %s signal", finger_name);
    return 0;
  }

  msg = finger_str_to_msg(finger_name, data->driver, data->is_swipe);
  if (!msg)
  {
    data->result = strdup("Protocol error with fprintd!");
    return 0;
  }
  if (debug)
    pam_syslog(data->pamh, LOG_DEBUG, "verify_finger_selected %s", msg);
  // send_info_msg (data->pamh, msg);
  return 0;
}

/* See https://github.com/systemd/systemd/issues/14636 */
static int
get_property_string(sd_bus *bus,
                    const char *destination,
                    const char *path,
                    const char *interface,
                    const char *member,
                    sd_bus_error *error,
                    char **ret)
{

  pf_autoptr(sd_bus_message) reply = NULL;
  const char *s;
  char *n;
  int r;

  r = sd_bus_call_method(bus, destination, path, "org.freedesktop.DBus.Properties", "Get", error, &reply, "ss", interface, member);
  if (r < 0)
    return r;

  r = sd_bus_message_enter_container(reply, 'v', "s");
  if (r < 0)
    return sd_bus_error_set_errno(error, r);

  r = sd_bus_message_read_basic(reply, 's', &s);
  if (r < 0)
    return sd_bus_error_set_errno(error, r);

  n = strdup(s);
  if (!n)
    return sd_bus_error_set_errno(error, -ENOMEM);

  *ret = n;
  return 0;
}

static int
verify_started_cb(sd_bus_message *m,
                  void *userdata,
                  sd_bus_error *ret_error)
{
  const sd_bus_error *error = sd_bus_message_get_error(m);
  verify_data *data = userdata;

  if (error)
  {
    if (sd_bus_error_has_name(error, "net.reactivated.Fprint.Error.NoEnrolledPrints"))
    {
      pam_syslog(data->pamh, LOG_DEBUG, "No prints enrolled");
      data->verify_ret = PAM_AUTHINFO_UNAVAIL;
    }
    else
    {
      data->verify_ret = PAM_AUTH_ERR;
    }

    if (debug)
      pam_syslog(data->pamh, LOG_DEBUG, "VerifyStart failed: %s", error->message);

    return 1;
  }

  if (debug)
    pam_syslog(data->pamh, LOG_DEBUG, "VerifyStart completed successfully");

  data->verify_started = true;

  return 1;
}

static void
fd_cleanup(int *fd)
{
  if (*fd >= 0)
    close(*fd);
}

typedef int fd_int;
PF_DEFINE_AUTO_CLEAN_FUNC(fd_int, fd_cleanup);

static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool has_recieved_sigusr1 = false;
static bool fingerprint_success = false;
static bool fingerprint_finished = false;

static void
handle_sigusr1(int sig)
{
  has_recieved_sigusr1 = true;
}

static int
do_verify(sd_bus *bus, verify_data *data)
{
  pf_autoptr(sd_bus_slot) verify_status_slot = NULL;
  pf_autoptr(sd_bus_slot) verify_finger_selected_slot = NULL;
  pf_autofree char *scan_type = NULL;
  sigset_t signals;
  fd_int signal_fd = -1;
  int r;
  int term_fd = -1;
  struct pollfd fds[2];

  if (!no_pthread)
    term_fd = -1;
  else
    term_fd = fileno(stdin);

  /* Get some properties for the device */
  r = get_property_string(bus,
                          "net.reactivated.Fprint",
                          data->dev,
                          "net.reactivated.Fprint.Device",
                          "scan-type",
                          NULL,
                          &scan_type);
  if (r < 0)
    pam_syslog(data->pamh, LOG_ERR, "Failed to get scan-type for %s: %d", data->dev, r);
  if (debug)
    pam_syslog(data->pamh, LOG_DEBUG, "scan-type for %s: %s", data->dev, scan_type);
  if (str_equal(scan_type, "swipe"))
    data->is_swipe = true;

  if (data->has_multiple_devices)
  {
    get_property_string(bus,
                        "net.reactivated.Fprint",
                        data->dev,
                        "net.reactivated.Fprint.Device",
                        "name",
                        NULL,
                        &data->driver);
    if (r < 0)
      pam_syslog(data->pamh, LOG_ERR, "Failed to get driver name for %s: %d", data->dev, r);
    if (debug && r == 0)
      pam_syslog(data->pamh, LOG_DEBUG, "driver name for %s: %s", data->dev, data->driver);
  }

  sd_bus_match_signal(bus,
                      &verify_status_slot,
                      "net.reactivated.Fprint",
                      data->dev,
                      "net.reactivated.Fprint.Device",
                      "VerifyStatus",
                      verify_result,
                      data);

  sd_bus_match_signal(bus,
                      &verify_finger_selected_slot,
                      "net.reactivated.Fprint",
                      data->dev,
                      "net.reactivated.Fprint.Device",
                      "VerifyFingerSelected",
                      verify_finger_selected,
                      data);

  if (!no_pthread)
  {
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGUSR1);
    signal_fd = signalfd(signal_fd, &signals, SFD_NONBLOCK);
  }

  while (data->max_tries > 0)
  {
    uint64_t verification_end = ULONG_MAX;

    if (timeout != UINT_MAX)
      verification_end = now() + (timeout * USEC_PER_SEC);

    data->timed_out = false;
    data->verify_started = false;
    data->verify_ret = PAM_INCOMPLETE;

    free(data->result);
    data->result = NULL;

    if (debug)
      pam_syslog(data->pamh, LOG_DEBUG, "About to call VerifyStart");

    r = sd_bus_call_method_async(bus,
                                 NULL,
                                 "net.reactivated.Fprint",
                                 data->dev,
                                 "net.reactivated.Fprint.Device",
                                 "VerifyStart",
                                 verify_started_cb,
                                 data,
                                 "s",
                                 "any");

    if (r < 0)
    {
      if (debug)
        pam_syslog(data->pamh, LOG_DEBUG, "VerifyStart call failed: %d", r);
      break;
    }

    for (;;)
    {
      struct signalfd_siginfo siginfo;
      int64_t wait_time;

      wait_time = verification_end - now();
      if (wait_time <= 0 || data->stop_got_pw)
        break;

      if (!no_pthread && read(signal_fd, &siginfo, sizeof(siginfo)) > 0)
      {
        if (debug)
          pam_syslog(data->pamh, LOG_DEBUG, "Received signal %d during verify", siginfo.ssi_signo);

        /* The only way for this to happen is if we received SIGINT. */
        return PAM_AUTHINFO_UNAVAIL;
      }

      r = sd_bus_process(bus, NULL);
      if (r < 0)
        break;
      if (data->verify_ret != PAM_INCOMPLETE)
        break;
      if (!data->verify_started)
        continue;
      if (data->result != NULL)
        break;
      if (r == 0)
      {
        int nfds = 1;
        fds[0].fd = sd_bus_get_fd(bus);
        fds[0].events = sd_bus_get_events(bus);
        fds[0].revents = 0;
        if (term_fd >= 0)
        {
          fds[1].fd = term_fd;
          fds[1].events = POLLIN;
          fds[1].revents = 0;
          nfds = 2;
        }

        // if (debug)
        //   {
        //     pam_syslog (data->pamh, LOG_DEBUG,
        //                 "Waiting for %" PRId64 " seconds (%" PRId64 " usecs)",
        //                 wait_time / USEC_PER_SEC,
        //                 wait_time);
        //   }

        r = poll(fds, nfds, wait_time / USEC_PER_MSEC);
        if (r < 0 && errno != EINTR)
        {
          pam_syslog(data->pamh, LOG_ERR, "Error waiting for events: %d", errno);
          return PAM_AUTHINFO_UNAVAIL;
        }

        // Check for keyboard input in no_pthread mode
        if (no_pthread && term_fd >= 0 && (fds[1].revents & POLLIN))
        {
          char c;
          if (read(term_fd, &c, 1) > 0)
          {
            if (debug)
              pam_syslog(data->pamh, LOG_DEBUG, "Key pressed during verify, stopping");
            return PAM_AUTHINFO_UNAVAIL;
          }
        }

        if (has_recieved_sigusr1 && !no_pthread)
        {
          if (debug)
            pam_syslog(data->pamh, LOG_DEBUG, "Got SIGUSR1: assuming pw recieved");
          return PAM_AUTHINFO_UNAVAIL;
        }
      }
    }

    if (data->verify_ret != PAM_INCOMPLETE)
      return data->verify_ret;

    if (now() >= verification_end && !no_need_enter && !no_pthread)
    {
      data->timed_out = true;
      send_err_msg(data->pamh, _("FP timeout"));
    }
    else
    {
      if (str_equal(data->result, "verify-no-match"))
        send_err_msg(data->pamh, _("FP no match, try again"));
      else if (str_equal(data->result, "verify-match"))
      {
        if (!no_pthread)
        {
          pthread_mutex_lock(&input_mutex);
          fingerprint_success = true;
          pthread_mutex_unlock(&input_mutex);
        }
        return PAM_SUCCESS;
      }
    }

    /* Ignore errors from VerifyStop */
    data->verify_started = false;
    (void)sd_bus_call_method(bus,
                             "net.reactivated.Fprint",
                             data->dev,
                             "net.reactivated.Fprint.Device",
                             "VerifyStop",
                             NULL,
                             NULL,
                             NULL,
                             NULL);

    if (data->timed_out || data->stop_got_pw)
    {
      return PAM_AUTHINFO_UNAVAIL;
    }
    else
    {
      if (str_equal(data->result, "verify-no-match"))
      {
        /* Nothing to do at this point. */
      }
      else if (str_equal(data->result, "verify-unknown-error"))
      {
        return PAM_AUTHINFO_UNAVAIL;
      }
      else if (str_equal(data->result, "verify-disconnected"))
      {
        return PAM_AUTHINFO_UNAVAIL;
      }
      else
      {
        send_err_msg(data->pamh, _("FP unknown error"));
        return PAM_AUTH_ERR;
      }
    }
    data->max_tries--;
  }

  if (data->max_tries == 0)
    return PAM_MAXTRIES;

  return PAM_AUTH_ERR;
}

static size_t
user_enrolled_prints_num(pam_handle_t *pamh,
                         sd_bus *bus,
                         const char *dev,
                         const char *username)
{
  pf_auto(sd_bus_error) error = SD_BUS_ERROR_NULL;
  pf_autoptr(sd_bus_message) m = NULL;
  size_t num_fingers = 0;
  const char *s;
  int r;

  r = sd_bus_call_method(bus,
                         "net.reactivated.Fprint",
                         dev,
                         "net.reactivated.Fprint.Device",
                         "ListEnrolledFingers",
                         &error,
                         &m,
                         "s",
                         username);
  if (r < 0)
  {
    /* If ListEnrolledFingers fails then verification should
     * also fail (both use the same underlying call), so we
     * report false here and bail out early.  */
    if (debug)
      pam_syslog(pamh, LOG_DEBUG, "ListEnrolledFingers failed for %s: %s",
                 username, error.message);
    return 0;
  }

  r = sd_bus_message_enter_container(m, 'a', "s");
  if (r < 0)
  {
    pam_syslog(pamh, LOG_ERR, "Failed to parse answer from ListEnrolledFingers(): %d", r);
    return 0;
  }

  while (sd_bus_message_read_basic(m, 's', &s) > 0)
    num_fingers++;
  sd_bus_message_exit_container(m);

  return num_fingers;
}

static void
release_device(pam_handle_t *pamh,
               sd_bus *bus,
               const char *dev)
{
  pf_auto(sd_bus_error) error = SD_BUS_ERROR_NULL;

  if (sd_bus_call_method(bus,
                         "net.reactivated.Fprint",
                         dev,
                         "net.reactivated.Fprint.Device",
                         "Release",
                         &error,
                         NULL,
                         NULL,
                         NULL) < 0)
    pam_syslog(pamh, LOG_ERR, "ReleaseDevice failed: %s", error.message);
}

static bool
claim_device(pam_handle_t *pamh,
             sd_bus *bus,
             const char *dev,
             const char *username)
{
  pf_auto(sd_bus_error) error = SD_BUS_ERROR_NULL;

  if (sd_bus_call_method(bus,
                         "net.reactivated.Fprint",
                         dev,
                         "net.reactivated.Fprint.Device",
                         "Claim",
                         &error,
                         NULL,
                         "s",
                         username) < 0)
  {
    if (debug)
      pam_syslog(pamh, LOG_DEBUG, "failed to claim device %s", error.message);
    return false;
  }

  return true;
}

static int
name_owner_changed(sd_bus_message *m,
                   void *userdata,
                   sd_bus_error *ret_error)
{
  verify_data *data = userdata;
  const char *name = NULL;
  const char *old_owner = NULL;
  const char *new_owner = NULL;

  if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0)
  {
    pam_syslog(data->pamh, LOG_ERR, "Failed to parse NameOwnerChanged signal: %d", errno);
    data->verify_ret = PAM_AUTHINFO_UNAVAIL;
    return 0;
  }

  if (strcmp(name, "net.reactivated.Fprint") != 0)
    return 0;

  /* Name owner for fprintd changed, give up as we might start listening
   * to events from a new name owner otherwise. */
  data->verify_ret = PAM_AUTHINFO_UNAVAIL;

  pam_syslog(data->pamh, LOG_WARNING, "fprintd name owner changed during operation!");

  if (debug)
    pam_syslog(data->pamh, LOG_DEBUG, "Old owner: %s, New owner: %s", old_owner ? old_owner : "-", new_owner ? new_owner : "-");

  return 0;
}

static void
connect_name_owner_changed(sd_bus *bus, verify_data *data, sd_bus_slot **name_owner_changed_slot)
{
  sd_bus_match_signal(bus,
                      name_owner_changed_slot,
                      "org.freedesktop.DBus",
                      "/org/freedesktop/DBus",
                      "org.freedesktop.DBus",
                      "NameOwnerChanged",
                      name_owner_changed,
                      data);
}

static void
disconnect_name_owner_changed(sd_bus *bus, sd_bus_slot **name_owner_changed_slot)
{
  if (*name_owner_changed_slot)
  {
    sd_bus_slot_unref(*name_owner_changed_slot);
    *name_owner_changed_slot = NULL;
  }
}

static void *
prompt_pw(void *d)
{
  verify_data *data = d;
  char *pw = NULL;
  int pam_result;
  const char *prompt_text;

  if (debug)
    pam_syslog(data->pamh, LOG_DEBUG, "Prompting for password");

  usleep(100000);

  pthread_mutex_lock(&input_mutex);
  if (fingerprint_success)
  {
    pthread_mutex_unlock(&input_mutex);
    if (debug)
      pam_syslog(data->pamh, LOG_DEBUG, "Fingerprint already succeeded, skipping password prompt");
    return NULL;
  }
  pthread_mutex_unlock(&input_mutex);

  // Set prompt text based on whether fingerprint is available
  if (data->fingerprint_enabled)
  {
    prompt_text = "Enter password (or scan fingerprint): ";
    if (debug)
      pam_syslog(data->pamh, LOG_DEBUG, "Using fingerprint-enabled prompt");
  }
  else
  {
    prompt_text = "Enter password: ";
    if (debug)
      pam_syslog(data->pamh, LOG_DEBUG, "Using password-only prompt");
  }

  // Use pam_prompt to get the password
  pam_result = pam_prompt(data->pamh, PAM_PROMPT_ECHO_OFF, &pw, "%s", prompt_text);
  data->pam_prompt_result = pam_result;

  if (debug)
    pam_syslog(data->pamh, LOG_DEBUG, "Pam prompt returned: %d", pam_result);

  // Check if we were interrupted or got NULL
  if (pam_result != PAM_SUCCESS || pw == NULL)
  {
    if (debug)
      pam_syslog(data->pamh, LOG_DEBUG, "No password received - likely fingerprint succeeded or error");
    pthread_mutex_lock(&input_mutex);
    if (fingerprint_finished)
    {
      pthread_mutex_unlock(&input_mutex);
      return NULL;
    }
    pthread_mutex_unlock(&input_mutex);
    // error while using password, let parent thread know
    data->stop_got_pw = true;
    kill(data->ppid, SIGUSR1);
    return NULL;
  }

  // Check again if fingerprint succeeded while we were waiting
  pthread_mutex_lock(&input_mutex);
  if (fingerprint_success)
  {
    pthread_mutex_unlock(&input_mutex);
    if (pw)
    {
      memset(pw, 0, strlen(pw));
      free(pw);
    }
    return NULL;
  }
  pthread_mutex_unlock(&input_mutex);

  // Process the password if received
  if (pw && *pw)
  {
    pam_set_item(data->pamh, PAM_AUTHTOK, pw);
  }

  data->stop_got_pw = true;
  if (debug)
    pam_syslog(data->pamh, LOG_DEBUG, "PW prompt done, setting stop_got_pw=true");

  // Signal to parent thread
  kill(data->ppid, SIGUSR1);

  // Clean up memory
  if (pw)
  {
    memset(pw, 0, strlen(pw)); // Clear password from memory
    free(pw);
  }
  return NULL;
}

static int do_auth_no_pthread(pam_handle_t *pamh, const char *username, sd_bus *bus, verify_data *data)
{
  int ret = PAM_AUTHINFO_UNAVAIL;
  bool in_pw_mode = pw_first;
  bool device_claimed = false;
  data->fingerprint_enabled = true; // assume we can still use fingerprint
  char *pw = NULL;
  struct termios term_attr;
  struct termios term_attr_old;
  int term_fd;

  // Get terminal settings for raw mode
  term_fd = fileno(stdin);
  if (tcgetattr(term_fd, &term_attr_old) == 0)
  {
    term_attr = term_attr_old;
    term_attr.c_lflag &= ~(ICANON | ECHO);
    term_attr.c_cc[VMIN] = 0;
    term_attr.c_cc[VTIME] = 1;
  }

  while (1)
  {
    if (in_pw_mode)
    {
      // Password mode
      if (debug)
        pam_syslog(pamh, LOG_DEBUG, "In password mode");

      // Restore normal terminal mode
      if (term_fd >= 0)
        tcsetattr(term_fd, TCSANOW, &term_attr_old);

      // Get password
      ret = pam_prompt(pamh, PAM_PROMPT_ECHO_OFF, &pw, data->fingerprint_enabled ? "Enter password (empty to switch to fingerprint): " : "Enter password: ");

      if (ret != PAM_SUCCESS)
      {
        return PAM_AUTH_ERR;
      }

      // Empty password means switch to fingerprint mode
      if (pw == NULL || *pw == '\0')
      {
        if (data->fingerprint_enabled)
        {
          in_pw_mode = false;
          if (pw)
          {
            memset(pw, 0, strlen(pw));
            free(pw);
            pw = NULL;
          }
          continue;
        }
        else
        {
          // No fingerprint device, treat as auth failure
          if (pw)
          {
            memset(pw, 0, strlen(pw));
            free(pw);
          }
          return PAM_AUTH_ERR;
        }
      }

      // Process password
      pam_set_item(pamh, PAM_AUTHTOK, pw);
      memset(pw, 0, strlen(pw));
      free(pw);
      return PAM_AUTHINFO_UNAVAIL; // Let other modules handle password
    }
    else
    {
      pf_autoptr(sd_bus_slot) name_owner_changed_slot = NULL;
      // Fingerprint mode
      if (data->fingerprint_enabled && !device_claimed)
      {
        pam_syslog(pamh, LOG_DEBUG, "Openning fingerprint device");
        data->dev = open_device(pamh, bus, username, &data->has_multiple_devices);
        if (data->dev == NULL)
        {
          if (debug)
            pam_syslog(pamh, LOG_DEBUG, "No device found, falling back to password");
          // Continue to password prompt even with no device
          data->fingerprint_enabled = false;
          pam_syslog(pamh, LOG_DEBUG, "Disabled fingerprint device");
        }
        else
        {
          device_claimed = claim_device(pamh, bus, data->dev, username);
          data->fingerprint_enabled = device_claimed;
          pam_syslog(pamh, LOG_DEBUG, "Claimed fingerprint device");
        }
      }

      if (!device_claimed)
      {
        in_pw_mode = true;
        continue;
      }

      connect_name_owner_changed(bus, data, &name_owner_changed_slot);

      if (debug)
        pam_syslog(pamh, LOG_DEBUG, "In fingerprint mode");

      // Set terminal to raw mode to detect keypress
      if (term_fd >= 0)
        tcsetattr(term_fd, TCSANOW, &term_attr);

      send_info_msg(pamh, _("Scan fingerprint or press any key to enter password"));

      // wait all keys released before running verify
      if (term_fd >= 0)
      {
        struct pollfd flush_fd = {term_fd, POLLIN, 0};
        char c;
        // First, read any pending input
        while (poll(&flush_fd, 1, 0) > 0 && (flush_fd.revents & POLLIN))
        {
          if (read(term_fd, &c, 1) <= 0)
            break;
        }
        // Then wait a short time to ensure no more keystrokes
        usleep(100000); // 100ms should be enough
        // Check one more time to ensure no new keystrokes
        while (poll(&flush_fd, 1, 0) > 0 && (flush_fd.revents & POLLIN))
        {
          if (read(term_fd, &c, 1) > 0)
          {
            // If we got more input, switch to password mode
            if (device_claimed)
            {
              release_device(pamh, bus, data->dev);
              data->dev = NULL;
            }
            in_pw_mode = true;
            if (debug)
              pam_syslog(pamh, LOG_DEBUG, "Key detected while flushing, switching to password mode");
            continue;
          }
          if (read(term_fd, &c, 1) <= 0)
            break;
        }
      }

      ret = do_verify(bus, data);

      disconnect_name_owner_changed(bus, &name_owner_changed_slot);

      pam_syslog(pamh, LOG_DEBUG, "Fprint returned %d", ret);

      if (ret == PAM_SUCCESS)
      {
        if (term_fd >= 0)
          tcsetattr(term_fd, TCSANOW, &term_attr_old);
        if (!no_need_enter)
        {
          send_info_msg(pamh, _("Fingerprint OK, press ENTER"));
          const char *dummy_pw = "";
          pam_set_item(pamh, PAM_AUTHTOK, dummy_pw);
        }
        return ret;
      }
      else
      {
        if (device_claimed)
        {
          pam_syslog(pamh, LOG_DEBUG, "Releasing fingerprint device");
          release_device(pamh, bus, data->dev);
          data->dev = NULL;
          device_claimed = false;
        }
        if (max_tries_switch_to_pw && ret == PAM_MAXTRIES)
        {
          in_pw_mode = true;
          data->max_tries = max_tries;
          if (debug)
            pam_syslog(pamh, LOG_DEBUG, "Max tries reached, switching to password mode");
          continue;
        }
        if (ret == PAM_AUTHINFO_UNAVAIL)
        {
          // This means either a key was pressed or verification was interrupted
          in_pw_mode = true;
          if (debug)
            pam_syslog(pamh, LOG_DEBUG, "Switching to password mode due to key press or interruption");
          continue;
        }
      }

      // If we reach here, verification failed with an error
      if (term_fd >= 0)
        tcsetattr(term_fd, TCSANOW, &term_attr_old);
      return ret;
    }
  }

  return PAM_AUTHINFO_UNAVAIL;
}

static int
do_auth(pam_handle_t *pamh, const char *username)
{
  pf_autoptr(verify_data) data = NULL;
  pf_autoptr(sd_bus) bus = NULL;
  int ret = PAM_AUTHINFO_UNAVAIL;

  data = calloc(1, sizeof(verify_data));
  data->max_tries = max_tries;
  data->pamh = pamh;
  data->fingerprint_enabled = false; // Initialize to false by default

  if (sd_bus_open_system(&bus) < 0)
  {
    pam_syslog(pamh, LOG_ERR, "Error with getting the bus: %d", errno);
    return PAM_AUTHINFO_UNAVAIL;
  }

  data->stop_got_pw = false;
  data->ppid = getpid();

  if (no_pthread)
  {
    return do_auth_no_pthread(pamh, username, bus, data);
  }
  else
  {
    pf_autoptr(sd_bus_slot) name_owner_changed_slot = NULL;
    bool device_claimed = false;
    bool device_need_release = true;
    data->dev = open_device(pamh, bus, username, &data->has_multiple_devices);
    if (data->dev == NULL)
    {
      if (debug)
        pam_syslog(pamh, LOG_DEBUG, "No device found, falling back to password");
      // Continue to password prompt even with no device
    }

    signal(SIGUSR1, handle_sigusr1);

    // Try to claim the device if available
    if (data->dev != NULL)
    {
      device_claimed = claim_device(pamh, bus, data->dev, username);
      if (debug && !device_claimed)
        pam_syslog(pamh, LOG_DEBUG, "Failed to claim device, falling back to password");

      // Set fingerprint_enabled flag if device is claimed successfully
      data->fingerprint_enabled = device_claimed;
      connect_name_owner_changed(bus, data, &name_owner_changed_slot);
    }

    // Always create password prompt thread
    pthread_t pw_prompt_thread;
    if (pthread_create(&pw_prompt_thread, NULL, prompt_pw, data) != 0)
    {
      pam_syslog(pamh, LOG_ERR, "Failed to create thread: %s", strerror(errno));
      if (device_claimed)
        release_device(pamh, bus, data->dev);
      sd_bus_close(bus);
      return PAM_SYSTEM_ERR;
    }

    // Only try fingerprint verification if device was claimed successfully
    if (device_claimed)
    {
      ret = do_verify(bus, data);
      disconnect_name_owner_changed(bus, &name_owner_changed_slot);
      if (ret == PAM_SUCCESS)
      {
        /* Logic from pam_fprintd: Simply disconnect from bus if we return PAM_SUCCESS */
        device_need_release = false;
      }

      pthread_mutex_lock(&input_mutex);
      fingerprint_finished = true;
      pthread_mutex_unlock(&input_mutex);
      if (debug)
        pam_syslog(pamh, LOG_DEBUG, "Verify returned %d", ret);

      if (data->stop_got_pw)
      {
        // result was set by the password prompt thread
        ret = data->pam_prompt_result == PAM_SUCCESS ? PAM_SUCCESS : PAM_AUTH_ERR;
      }
      else if (ret == PAM_SUCCESS)
      {
        if (!no_need_enter)
        {
          send_info_msg(pamh, _("Fingerprint OK, press ENTER"));
          // Set a dummy password to indicate success
          const char *dummy_pw = "";
          pam_set_item(pamh, PAM_AUTHTOK, dummy_pw);
        }
      }
      else
      {
        if (debug)
          pam_syslog(pamh, LOG_DEBUG, "Verify returned %d, tell user to input password", ret);
        send_err_msg(pamh, _("Enter password"));
      }
    }

    if (no_need_enter)
    {
      pthread_cancel(pw_prompt_thread);
    }
    // Wait for the password prompt thread to complete
    pthread_join(pw_prompt_thread, NULL);
    if (debug)
      pam_syslog(pamh, LOG_DEBUG, "PW prompt thread joined");

    // Check if we got a password
    if (data->stop_got_pw)
    {
      if (debug)
        pam_syslog(pamh, LOG_DEBUG, "Authentication continues with password");
      if (data->pam_prompt_result == PAM_SUCCESS)
      {
        ret = PAM_AUTHINFO_UNAVAIL; // Let other modules handle the password
      }
      else
      {
        ret = PAM_AUTH_ERR;
      }
    }
    if (device_claimed && device_need_release)
      release_device(pamh, bus, data->dev);
  }

  sd_bus_close(bus);

  if (debug)
    pam_syslog(pamh, LOG_DEBUG, "Returning %d", ret);
  return ret;
}

static bool
is_remote(pam_handle_t *pamh)
{
  const char *rhost = NULL;

  pam_get_item(pamh, PAM_RHOST, (const void **)(const void *)&rhost);

  /* NULL or empty rhost if the host information is not available or set.
   * "localhost" if the host is local.
   * We want to not run for known remote hosts */
  if (rhost != NULL &&
      *rhost != '\0' &&
      strcmp(rhost, "localhost") != 0)
    return true;

  if (sd_session_is_remote(NULL) > 0)
    return true;

  return false;
}

PAM_EXTERN int
pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc,
                    const char **argv)
{
  const char *username;
  int i;

  bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");

  if (is_remote(pamh))
    return PAM_AUTHINFO_UNAVAIL;

  if (pam_get_user(pamh, &username, NULL) != PAM_SUCCESS)
    return PAM_AUTHINFO_UNAVAIL;

  for (i = 0; i < argc; i++)
  {
    if (argv[i] != NULL)
    {
      if (str_equal(argv[i], "debug"))
      {
        pam_syslog(pamh, LOG_DEBUG, "debug on");
        debug = true;
      }
      else if (str_equal(argv[i], FP_MAX_TRIES_SWITCH_TO_PW))
      {
        pam_syslog(pamh, LOG_DEBUG, "Fingerprint max tries switch to password");
        max_tries_switch_to_pw = true;
      }
      else if (str_has_prefix(argv[i], DEBUG_MATCH))
      {
        pam_syslog(pamh, LOG_DEBUG, "debug on");
        const char *value;

        value = argv[i] + strlen(DEBUG_MATCH);
        if (str_equal(value, "on") ||
            str_equal(value, "true") ||
            str_equal(value, "1"))
        {
          pam_syslog(pamh, LOG_DEBUG, "debug on");
          debug = true;
        }
        else if (str_equal(value, "off") ||
                 str_equal(value, "false") ||
                 str_equal(value, "0"))
        {
          debug = false;
        }
        else
        {
          pam_syslog(pamh, LOG_DEBUG, "invalid debug value '%s', disabling", value);
        }
      }
      else if (str_has_prefix(argv[i], MAX_TRIES_MATCH) && strlen(argv[i]) > strlen(MAX_TRIES_MATCH))
      {
        int opt_max_tries = atoi(argv[i] + strlen(MAX_TRIES_MATCH));
        max_tries = (opt_max_tries < 0 ? UINT_MAX : (unsigned)opt_max_tries);
        if (max_tries < 1)
        {
          if (debug)
            pam_syslog(pamh, LOG_DEBUG, "invalid max tries '%s', using %d",
                       argv[i] + strlen(MAX_TRIES_MATCH), DEFAULT_MAX_TRIES);
          max_tries = DEFAULT_MAX_TRIES;
        }
        if (debug)
          pam_syslog(pamh, LOG_DEBUG, "max_tries specified as: %d", max_tries);
      }
      else if (str_has_prefix(argv[i], TIMEOUT_MATCH) && strlen(argv[i]) <= strlen(TIMEOUT_MATCH) + 2)
      {
        int opt_timeout = atoi(argv[i] + strlen(TIMEOUT_MATCH));
        timeout = (opt_timeout < 0 ? UINT_MAX : (unsigned)opt_timeout);
        if (timeout < MIN_TIMEOUT)
        {
          if (debug)
            pam_syslog(pamh, LOG_DEBUG, "timeout %d secs too low, using %d",
                       timeout, MIN_TIMEOUT);
          timeout = MIN_TIMEOUT;
        }
        else if (debug)
        {
          pam_syslog(pamh, LOG_DEBUG, "timeout specified as: %d secs", timeout);
        }
      }
      else if (str_has_prefix(argv[i], NO_NEED_ENTER_MATCH) && strlen(argv[i]) <= strlen(NO_NEED_ENTER_MATCH) + 2)
      {
        no_need_enter = true;
      }
      else if (str_has_prefix(argv[i], NO_PTHREAD_MATCH) && strlen(argv[i]) <= strlen(NO_PTHREAD_MATCH) + 2)
      {
        no_pthread = true;
      }
      else if (str_has_prefix(argv[i], NO_PTHREAD_PW_FIRST_MATCH) && strlen(argv[i]) <= strlen(NO_PTHREAD_PW_FIRST_MATCH) + 2)
      {
        no_pthread = true;
        pw_first = true;
      }
    }
  }

  if (no_pthread)
  {
    no_need_enter = true;
  }

  return do_auth(pamh, username);
}

PAM_EXTERN int
pam_sm_setcred(pam_handle_t *pamh, int flags, int argc,
               const char **argv)
{
  return PAM_SUCCESS;
}

PAM_EXTERN int
pam_sm_chauthtok(pam_handle_t *pamh, int flags, int argc,
                 const char **argv)
{
  return PAM_SUCCESS;
}
