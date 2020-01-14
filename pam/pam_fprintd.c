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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <syslog.h>
#include <errno.h>

#include <libintl.h>
#include <systemd/sd-bus.h>

#define PAM_SM_AUTH
#include <security/pam_modules.h>
#include <security/pam_ext.h>

#define _(s) ((char *) dgettext (GETTEXT_PACKAGE, s))
#define TR(s) dgettext(GETTEXT_PACKAGE, s)
#define N_(s) (s)

#include "fingerprint-strings.h"

#define DEFAULT_MAX_TRIES 3
#define DEFAULT_TIMEOUT 30

#define MAX_TRIES_MATCH "max-tries="
#define TIMEOUT_MATCH "timeout="

static bool debug = false;
static unsigned max_tries = DEFAULT_MAX_TRIES;
static unsigned timeout = DEFAULT_TIMEOUT;

#define USEC_PER_SEC ((uint64_t) 1000000ULL)
#define NSEC_PER_USEC ((uint64_t) 1000ULL)

static uint64_t
now (void)
{
	struct timespec ts;
	clock_gettime (CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * USEC_PER_SEC + (uint64_t) ts.tv_nsec / NSEC_PER_USEC;
}

static bool str_has_prefix (const char *s, const char *prefix)
{
	if (s == NULL || prefix == NULL)
		return false;
	return (strncmp (s, prefix, strlen (prefix)) == 0);
}

static bool send_msg(pam_handle_t *pamh, const char *msg, int style)
{
	const struct pam_message mymsg = {
		.msg_style = style,
		.msg = msg,
	};
	const struct pam_message *msgp = &mymsg;
	const struct pam_conv *pc;
	struct pam_response *resp;
	int r;

	r = pam_get_item(pamh, PAM_CONV, (const void **) &pc);
	if (r != PAM_SUCCESS)
		return false;

	if (!pc || !pc->conv)
		return false;

	return (pc->conv(1, &msgp, &resp, pc->appdata_ptr) == PAM_SUCCESS);
}

static bool send_info_msg(pam_handle_t *pamh, const char *msg)
{
	return send_msg(pamh, msg, PAM_TEXT_INFO);
}

static bool send_err_msg(pam_handle_t *pamh, const char *msg)
{
	return send_msg(pamh, msg, PAM_ERROR_MSG);
}

static char *
open_device (pam_handle_t    *pamh,
	     sd_bus          *bus,
	     bool            *has_multiple_devices)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *m = NULL;
	size_t num_devices;
	const char *path = NULL;
	char *ret;
	const char *s;
	int r;

	*has_multiple_devices = false;

	r = sd_bus_call_method (bus,
				"net.reactivated.Fprint",
				"/net/reactivated/Fprint/Manager",
				"net.reactivated.Fprint.Manager",
				"GetDevices",
				&error,
				&m,
				NULL);
	if (r < 0) {
		pam_syslog (pamh, LOG_ERR, "GetDevices failed: %s", error.message);
		sd_bus_error_free (&error);
		return NULL;
	}

	r = sd_bus_message_enter_container (m, 'a', "o");
	if (r < 0) {
		pam_syslog (pamh, LOG_ERR, "Failed to parse answer from GetDevices(): %d", r);
		goto out;
	}

	r = sd_bus_message_read_basic (m, 'o', &path);
	if (r < 0)
		goto out;

	num_devices = 1;
	while ((r = sd_bus_message_read_basic(m, 'o', &s)) > 0)
		num_devices++;
	*has_multiple_devices = (num_devices > 1);
	if (debug)
		pam_syslog(pamh, LOG_DEBUG, "Using device %s (out of %ld devices)", path, num_devices);

	sd_bus_message_exit_container (m);

out:
	ret = path ? strdup (path) : NULL;
	sd_bus_message_unref (m);
	return ret;
}

typedef struct {
	unsigned max_tries;
	char *result;
	bool timed_out;
	bool is_swipe;
	pam_handle_t *pamh;

	char *driver;
} verify_data;

static int
verify_result (sd_bus_message *m,
	       void           *userdata,
	       sd_bus_error   *ret_error)
{
	verify_data *data = userdata;
	const char *msg;
	const char *result = NULL;
	/* see https://github.com/systemd/systemd/issues/14643 */
	uint64_t done = false;
	int r;

	if (!sd_bus_message_is_signal(m, "net.reactivated.Fprint.Device", "VerifyStatus")) {
		pam_syslog (data->pamh, LOG_ERR, "Not the signal we expected (iface: %s, member: %s)",
			    sd_bus_message_get_interface (m),
			    sd_bus_message_get_member (m));
		return 0;
	}

	if ((r = sd_bus_message_read (m, "sb", &result, &done)) < 0) {
		pam_syslog (data->pamh, LOG_ERR, "Failed to parse VerifyResult signal: %d", r);
		return 0;
	}

	if (debug)
		pam_syslog (data->pamh, LOG_DEBUG, "Verify result: %s (done: %d)", result, done ? 1 : 0);

	if (done) {
		data->result = strdup (result);
		return 0;
	}

	msg = _(verify_result_str_to_msg (result, data->is_swipe));
	send_err_msg (data->pamh, msg);

	return 0;
}

static int
verify_finger_selected (sd_bus_message *m,
			void           *userdata,
			sd_bus_error   *ret_error)
{
	verify_data *data = userdata;
	const char *finger_name = NULL;
	char *msg;

	if (sd_bus_message_read_basic (m, 's', &finger_name) < 0) {
		pam_syslog (data->pamh, LOG_ERR, "Failed to parse VerifyFingerSelected signal: %m");
		return 0;
	}

	msg = finger_str_to_msg(finger_name, data->driver, data->is_swipe);
	if (debug)
		pam_syslog (data->pamh, LOG_DEBUG, "verify_finger_selected %s", msg);
	send_info_msg (data->pamh, msg);
	free (msg);

	return 0;
}

/* See https://github.com/systemd/systemd/issues/14636 */
static int
get_property_string (sd_bus *bus,
		     const char *destination,
		     const char *path,
		     const char *interface,
		     const char *member,
		     sd_bus_error *error,
		     char **ret) {

	sd_bus_message *reply = NULL;
	const char *s;
	char *n;
	int r;

	r = sd_bus_call_method(bus, destination, path, "org.freedesktop.DBus.Properties", "Get", error, &reply, "ss", interface, member);
	if (r < 0)
		return r;

	r = sd_bus_message_enter_container(reply, 'v', "s");
	if (r < 0)
		goto fail;

	r = sd_bus_message_read_basic(reply, 's', &s);
	if (r < 0)
		goto fail;

	n = strdup(s);
	if (!n) {
		r = -ENOMEM;
		goto fail;
	}

	sd_bus_message_unref (reply);

	*ret = n;
	return 0;

fail:
	if (reply != NULL)
		sd_bus_message_unref (reply);
	return sd_bus_error_set_errno(error, r);
}

static int
do_verify (pam_handle_t *pamh,
	   sd_bus       *bus,
	   const char   *dev,
	   bool          has_multiple_devices)
{
	verify_data *data;
	sd_bus_slot *verify_status_slot, *verify_finger_selected_slot;
	char *scan_type = NULL;
	int ret;
	int r;

	data = calloc (1, sizeof(verify_data));
	data->max_tries = max_tries;
	data->pamh = pamh;

	/* Get some properties for the device */
	r = get_property_string (bus,
				 "net.reactivated.Fprint",
				 dev,
				 "net.reactivated.Fprint.Device",
				 "scan-type",
				 NULL,
				 &scan_type);
	if (r < 0)
		pam_syslog (data->pamh, LOG_ERR, "Failed to get scan-type for %s: %d", dev, r);
	if (debug)
		pam_syslog (data->pamh, LOG_DEBUG, "scan-type for %s: %s", dev, scan_type);
	if (str_equal (scan_type, "swipe"))
		data->is_swipe = true;
	free (scan_type);

	if (has_multiple_devices) {
		get_property_string (bus,
				     "net.reactivated.Fprint",
				     dev,
				     "net.reactivated.Fprint.Device",
				     "name",
				     NULL,
				     &data->driver);
		if (r < 0)
			pam_syslog (data->pamh, LOG_ERR, "Failed to get driver name for %s: %d", dev, r);
		if (debug && r == 0)
			pam_syslog (data->pamh, LOG_DEBUG, "driver name for %s: %s", dev, data->driver);
	}

	verify_status_slot = NULL;
	sd_bus_match_signal (bus,
			     &verify_status_slot,
			     "net.reactivated.Fprint",
			     dev,
			     "net.reactivated.Fprint.Device",
			     "VerifyStatus",
			     verify_result,
			     data);

	verify_finger_selected_slot = NULL;
	sd_bus_match_signal (bus,
			     &verify_finger_selected_slot,
			     "net.reactivated.Fprint",
			     dev,
			     "net.reactivated.Fprint.Device",
			     "VerifyFingerSelected",
			     verify_finger_selected,
			     data);

	ret = PAM_AUTH_ERR;

	while (ret == PAM_AUTH_ERR && data->max_tries > 0) {
		uint64_t verification_end = now () + (timeout * USEC_PER_SEC);
		sd_bus_message *m = NULL;
		sd_bus_error error = SD_BUS_ERROR_NULL;

		data->timed_out = false;

		r = sd_bus_call_method (bus,
					"net.reactivated.Fprint",
					dev,
					"net.reactivated.Fprint.Device",
					"VerifyStart",
					&error,
					&m,
					"s",
					"any");

		if (r < 0) {
			if (sd_bus_error_has_name (&error, "net.reactivated.Fprint.Error.NoEnrolledPrints"))
				ret = PAM_USER_UNKNOWN;

			if (debug)
				pam_syslog (pamh, LOG_DEBUG, "VerifyStart failed: %s", error.message);
			sd_bus_error_free (&error);
			break;
		}

		for (;;) {
			int64_t wait_time;

			wait_time = verification_end - now();
			if (wait_time <= 0)
				break;

			r = sd_bus_process (bus, NULL);
			if (r < 0)
				break;
			if (data->result != NULL)
				break;
			if (r == 0) {
				if (debug) {
					pam_syslog(pamh, LOG_DEBUG, "Waiting for %"PRId64" seconds (%"PRId64" usecs)",
						   wait_time / USEC_PER_SEC,
						   wait_time);
				}
				r = sd_bus_wait (bus, wait_time);
				if (r < 0)
					break;
			}
		}

		if (now () >= verification_end) {
			data->timed_out = true;
			send_info_msg (data->pamh, _("Verification timed out"));
		}

		/* Ignore errors from VerifyStop */
		sd_bus_call_method (bus,
				    "net.reactivated.Fprint",
				    dev,
				    "net.reactivated.Fprint.Device",
				    "VerifyStop",
				    NULL,
				    NULL,
				    NULL,
				    NULL);

		if (data->timed_out) {
			ret = PAM_AUTHINFO_UNAVAIL;
			break;
		} else {
			if (str_equal (data->result, "verify-no-match")) {
				send_err_msg (data->pamh, "Failed to match fingerprint");
				ret = PAM_AUTH_ERR;
			} else if (str_equal (data->result, "verify-match")) {
				ret = PAM_SUCCESS;
			} else if (str_equal (data->result, "verify-unknown-error")) {
				ret = PAM_AUTHINFO_UNAVAIL;
			} else if (str_equal (data->result, "verify-disconnected")) {
				ret = PAM_AUTHINFO_UNAVAIL;
				free (data->result);
				break;
			} else {
				send_info_msg (data->pamh, _("An unknown error occurred"));
				ret = PAM_AUTH_ERR;
				free (data->result);
				break;
			}
			free (data->result);
			data->result = NULL;
		}
		data->max_tries--;
	}

	sd_bus_slot_unref (verify_status_slot);
	sd_bus_slot_unref (verify_finger_selected_slot);

	free (data->driver);
	free (data);

	return ret;
}

static bool
user_has_prints (pam_handle_t *pamh,
		 sd_bus       *bus,
		 const char   *dev,
		 const char   *username)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *m = NULL;
	size_t num_fingers = 0;
	const char *s;
	int r;

	r = sd_bus_call_method (bus,
				"net.reactivated.Fprint",
				dev,
				"net.reactivated.Fprint.Device",
				"ListEnrolledFingers",
				&error,
				&m,
				"s",
				username);
	if (r < 0) {
		/* If ListEnrolledFingers fails then verification should
		 * also fail (both use the same underlying call), so we
		 * report false here and bail out early.  */
		if (debug) {
			pam_syslog (pamh, LOG_DEBUG, "ListEnrolledFingers failed for %s: %s",
				    username, error.message);
		}
		sd_bus_error_free (&error);
		return false;
	}

	r = sd_bus_message_enter_container (m, 'a', "s");
	if (r < 0) {
		pam_syslog (pamh, LOG_ERR, "Failed to parse answer from ListEnrolledFingers(): %d", r);
		goto out;
	}

	num_fingers = 0;
	while ((r = sd_bus_message_read_basic(m, 's', &s)) > 0)
		num_fingers++;
	sd_bus_message_exit_container (m);

out:
	sd_bus_message_unref (m);
	return (num_fingers > 0);
}

static void
release_device (pam_handle_t *pamh,
		 sd_bus       *bus,
		 const char   *dev)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *m = NULL;
	int r;

	r = sd_bus_call_method (bus,
				"net.reactivated.Fprint",
				dev,
				"net.reactivated.Fprint.Device",
				"Release",
				&error,
				&m,
				NULL,
				NULL);
	if (r < 0) {
		pam_syslog (pamh, LOG_ERR, "ReleaseDevice failed: %s", error.message);
		sd_bus_error_free (&error);
		return;
	}

	//FIXME needed?
	sd_bus_message_unref (m);
}

static bool
claim_device (pam_handle_t *pamh,
	      sd_bus       *bus,
	      const char   *dev,
	      const char   *username)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *m = NULL;
	int r;

	r = sd_bus_call_method (bus,
				"net.reactivated.Fprint",
				dev,
				"net.reactivated.Fprint.Device",
				"Claim",
				&error,
				&m,
				"s",
				username);
	if (r < 0) {
		if (debug)
			pam_syslog (pamh, LOG_DEBUG, "failed to claim device %s", error.message);
		sd_bus_error_free (&error);
		return false;
	}

	//FIXME needed?
	sd_bus_message_unref (m);

	return true;
}

static int do_auth(pam_handle_t *pamh, const char *username)
{
	char *dev;
	bool have_prints;
	bool has_multiple_devices;
	int ret = PAM_AUTHINFO_UNAVAIL;
	sd_bus *bus = NULL;

	if (sd_bus_open_system (&bus) < 0) {
		pam_syslog (pamh, LOG_ERR, "Error with getting the bus: %m");
		return PAM_AUTHINFO_UNAVAIL;
	}

	dev = open_device (pamh, bus, &has_multiple_devices);
	if (dev == NULL) {
		sd_bus_unref (bus);
		return PAM_AUTHINFO_UNAVAIL;
	}

	have_prints = user_has_prints (pamh, bus, dev, username);
	if (debug)
		pam_syslog (pamh, LOG_DEBUG, "prints registered: %s\n", have_prints ? "yes" : "no");

	if (!have_prints)
		goto out;

	if (claim_device (pamh, bus, dev, username)) {
		ret = do_verify (pamh, bus, dev, has_multiple_devices);
		release_device (pamh, bus, dev);
	}

out:
	free (dev);
	sd_bus_unref (bus);

	return ret;
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc,
				   const char **argv)
{
	const char *rhost = NULL;
	const char *username;
	unsigned i;
	int r;

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

	pam_get_item(pamh, PAM_RHOST, (const void **)(const void*) &rhost);

	/* NULL or empty rhost if the host information is not available or set.
	 * "localhost" if the host is local.
	 * We want to not run for known remote hosts */
	if (rhost != NULL &&
	    *rhost != '\0' &&
	    strcmp (rhost, "localhost") != 0) {
		return PAM_AUTHINFO_UNAVAIL;
	}

	r = pam_get_user(pamh, &username, NULL);
	if (r != PAM_SUCCESS)
		return PAM_AUTHINFO_UNAVAIL;

	for (i = 0; i < argc; i++) {
		if (argv[i] != NULL) {
			if (str_equal (argv[i], "debug")) {
				pam_syslog (pamh, LOG_DEBUG, "debug on");
				debug = true;
			} else if (str_has_prefix (argv[i], MAX_TRIES_MATCH) && strlen(argv[i]) == strlen (MAX_TRIES_MATCH) + 1) {
				max_tries = atoi (argv[i] + strlen (MAX_TRIES_MATCH));
				if (max_tries < 1)
					max_tries = DEFAULT_MAX_TRIES;
				if (debug)
					pam_syslog (pamh, LOG_DEBUG, "max_tries specified as: %d", max_tries);
			} else if (str_has_prefix (argv[i], TIMEOUT_MATCH) && strlen(argv[i]) <= strlen (TIMEOUT_MATCH) + 2) {
				timeout = atoi (argv[i] + strlen (TIMEOUT_MATCH));
				if (timeout < 10)
					timeout = DEFAULT_TIMEOUT;
				if (debug)
					pam_syslog (pamh, LOG_DEBUG, "timeout specified as: %d", timeout);
			}
		}
	}

	r = do_auth(pamh, username);

	return r;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc,
			      const char **argv)
{
	return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_chauthtok(pam_handle_t *pamh, int flags, int argc,
				const char **argv)
{
	return PAM_SUCCESS;
}

