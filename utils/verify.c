/*
 * fprintd example to verify a fingerprint
 * Copyright (C) 2008 Daniel Drake <dsd@gentoo.org>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dbus/dbus-glib-bindings.h>
#include "manager-dbus-glue.h"
#include "device-dbus-glue.h"
#include "marshal.h"

static DBusGProxy *manager = NULL;
static DBusGConnection *connection = NULL;
static char *finger_name = NULL;
static gboolean g_fatal_warnings = FALSE;
static char **usernames = NULL;

static void create_manager(void)
{
	GError *error = NULL;

	connection = dbus_g_bus_get(DBUS_BUS_SYSTEM, &error);
	if (connection == NULL) {
		g_print("Failed to connect to session bus: %s\n", error->message);
		exit (1);
	}

	manager = dbus_g_proxy_new_for_name(connection,
		"net.reactivated.Fprint", "/net/reactivated/Fprint/Manager",
		"net.reactivated.Fprint.Manager");
}

static DBusGProxy *open_device(const char *username)
{
	GError *error = NULL;
	gchar *path;
	DBusGProxy *dev;

	if (!net_reactivated_Fprint_Manager_get_default_device(manager, &path, &error)) {
		g_print("list_devices failed: %s\n", error->message);
		exit (1);
	}
	
	if (path == NULL) {
		g_print("No devices found\n");
		exit(1);
	}

	g_print("Using device %s\n", path);

	/* FIXME use for_name_owner?? */
	dev = dbus_g_proxy_new_for_name(connection, "net.reactivated.Fprint",
		path, "net.reactivated.Fprint.Device");
	
	g_free (path);

	if (!net_reactivated_Fprint_Device_claim(dev, username, &error)) {
		g_print("failed to claim device: %s\n", error->message);
		exit (1);
	}

	return dev;
}

static void find_finger(DBusGProxy *dev, const char *username)
{
	GError *error = NULL;
	char **fingers;
	guint i;

	if (!net_reactivated_Fprint_Device_list_enrolled_fingers(dev, username, &fingers, &error)) {
		g_print("ListEnrolledFingers failed: %s\n", error->message);
		exit (1);
	}

	if (fingers == NULL || g_strv_length (fingers) == 0) {
		g_print("No fingers enrolled for this device.\n");
		exit(1);
	}

	g_print("Listing enrolled fingers:\n");
	for (i = 0; fingers[i] != NULL; i++) {
		g_print(" - #%d: %s\n", i, fingers[i]);
	}

	if (finger_name == NULL || strcmp (finger_name, "any") == 0) {
		g_free (finger_name);
		finger_name = g_strdup (fingers[0]);
	}

	g_strfreev (fingers);
}

static void verify_result(GObject *object, const char *result, gboolean done, void *user_data)
{
	gboolean *verify_completed = user_data;
	g_print("Verify result: %s (%s)\n", result, done ? "done" : "not done");
	if (done != FALSE)
		*verify_completed = TRUE;
}

static void verify_finger_selected(GObject *object, const char *name, void *user_data)
{
	g_print("Verifying: %s\n", name);
}

static void do_verify(DBusGProxy *dev)
{
	GError *error = NULL;
	gboolean verify_completed = FALSE;

	dbus_g_proxy_add_signal(dev, "VerifyStatus", G_TYPE_STRING, G_TYPE_BOOLEAN, NULL);
	dbus_g_proxy_add_signal(dev, "VerifyFingerSelected", G_TYPE_INT, NULL);
	dbus_g_proxy_connect_signal(dev, "VerifyStatus", G_CALLBACK(verify_result),
				    &verify_completed, NULL);
	dbus_g_proxy_connect_signal(dev, "VerifyFingerSelected", G_CALLBACK(verify_finger_selected),
		NULL, NULL);

	if (!net_reactivated_Fprint_Device_verify_start(dev, finger_name, &error)) {
		g_print("VerifyStart failed: %s\n", error->message);
		exit (1);
	}

	while (!verify_completed)
		g_main_context_iteration(NULL, TRUE);

	dbus_g_proxy_disconnect_signal(dev, "VerifyStatus", G_CALLBACK(verify_result), &verify_completed);
	dbus_g_proxy_disconnect_signal(dev, "VerifyFingerSelected", G_CALLBACK(verify_finger_selected), NULL);

	if (!net_reactivated_Fprint_Device_verify_stop(dev, &error)) {
		g_print("VerifyStop failed: %s\n", error->message);
		exit (1);
	}
}

static void release_device(DBusGProxy *dev)
{
	GError *error = NULL;
	if (!net_reactivated_Fprint_Device_release(dev, &error)) {
		g_print("ReleaseDevice failed: %s\n", error->message);
		exit (1);
	}
}

static const GOptionEntry entries[] = {
	{ "finger", 'f',  0, G_OPTION_ARG_STRING, &finger_name, "Finger selected to verify (default is automatic)", NULL },
	{"g-fatal-warnings", 0, 0, G_OPTION_ARG_NONE, &g_fatal_warnings, "Make all warnings fatal", NULL},
 	{ G_OPTION_REMAINING, '\0', 0, G_OPTION_ARG_STRING_ARRAY, &usernames, NULL, "[username]" },
	{ NULL }
};

int main(int argc, char **argv)
{
	GOptionContext *context;
	GError *err = NULL;
	DBusGProxy *dev;
	char *username;

#if !GLIB_CHECK_VERSION (2, 36, 0)
	g_type_init();
#endif

	dbus_g_object_register_marshaller (fprintd_marshal_VOID__STRING_BOOLEAN,
					   G_TYPE_NONE, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_INVALID);

	context = g_option_context_new ("Verify a fingerprint");
	g_option_context_add_main_entries (context, entries, NULL);

	if (g_option_context_parse (context, &argc, &argv, &err) == FALSE) {
		g_print ("couldn't parse command-line options: %s\n", err->message);
		g_error_free (err);
		return 1;
	}

	if (usernames == NULL) {
		username = "";
	} else {
		username = usernames[0];
	}

	if (g_fatal_warnings) {
		GLogLevelFlags fatal_mask;

		fatal_mask = g_log_set_always_fatal (G_LOG_FATAL_MASK);
		fatal_mask |= G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL;
		g_log_set_always_fatal (fatal_mask);
	}

	create_manager();

	dev = open_device(username);
	find_finger(dev, username);
	do_verify(dev);
	release_device(dev);
	return 0;
}

