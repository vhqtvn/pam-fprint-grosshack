/*
 * fprintd example to enroll right index finger
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

#include <stdlib.h>
#include <string.h>
#include <dbus/dbus-glib-bindings.h>
#include "manager-dbus-glue.h"
#include "device-dbus-glue.h"
#include "marshal.h"

#define N_(x) x
#define TR(x) x
#include "fingerprint-strings.h"

static DBusGProxy *manager = NULL;
static DBusGConnection *connection = NULL;
static char *finger_name = "right-index-finger";
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

static void enroll_result(GObject *object, const char *result, gboolean done, void *user_data)
{
	gboolean *enroll_completed = user_data;
	g_print("Enroll result: %s\n", result);
	if (done != FALSE)
		*enroll_completed = TRUE;
}

static void do_enroll(DBusGProxy *dev)
{
	GError *error = NULL;
	gboolean enroll_completed = FALSE;
	gboolean found;
	guint i;

	dbus_g_proxy_add_signal(dev, "EnrollStatus", G_TYPE_STRING, G_TYPE_BOOLEAN, NULL);
	dbus_g_proxy_connect_signal(dev, "EnrollStatus", G_CALLBACK(enroll_result),
				    &enroll_completed, NULL);

	found = FALSE;
	for (i = 0; fingers[i].dbus_name != NULL; i++) {
		if (g_strcmp0 (fingers[i].dbus_name, finger_name) == 0) {
			found = TRUE;
			break;
		}
	}
	if (!found) {
		GString *s;

		s = g_string_new (NULL);
		g_string_append_printf (s, "Invalid finger name '%s'. Name must be one of ", finger_name);
		for (i = 0; fingers[i].dbus_name != NULL; i++) {
			g_string_append_printf (s, "%s", fingers[i].dbus_name);
			if (fingers[i + 1].dbus_name != NULL)
				g_string_append (s, ", ");
		}
		g_warning ("%s", s->str);
		g_string_free (s, TRUE);
		exit (1);
	}

	g_print("Enrolling %s finger.\n", finger_name);
	if (!net_reactivated_Fprint_Device_enroll_start(dev, finger_name, &error)) {
		g_print("EnrollStart failed: %s\n", error->message);
		exit (1);
	}

	while (!enroll_completed)
		g_main_context_iteration(NULL, TRUE);

	dbus_g_proxy_disconnect_signal(dev, "EnrollStatus",
		G_CALLBACK(enroll_result), &enroll_completed);

	if (!net_reactivated_Fprint_Device_enroll_stop(dev, &error)) {
		g_print("VerifyStop failed: %s\n", error->message);
		exit(1);
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
    { G_OPTION_REMAINING, '\0', 0, G_OPTION_ARG_STRING_ARRAY, &usernames, NULL, "[username]" },
    { NULL }
};

int main(int argc, char **argv)
{
	GOptionContext *context;
	GError *err = NULL;
	DBusGProxy *dev;

#if !GLIB_CHECK_VERSION (2, 36, 0)
	g_type_init();
#endif

	dbus_g_object_register_marshaller (fprintd_marshal_VOID__STRING_BOOLEAN,
					   G_TYPE_NONE, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_INVALID);

	context = g_option_context_new ("Enroll a fingerprint");
	g_option_context_add_main_entries (context, entries, NULL);

	if (g_option_context_parse (context, &argc, &argv, &err) == FALSE) {
		g_print ("couldn't parse command-line options: %s\n", err->message);
		g_error_free (err);
		return 1;
	}

	create_manager();

	dev = open_device(usernames ? usernames[0] : NULL);
	do_enroll(dev);
	release_device(dev);
	return 0;
}

