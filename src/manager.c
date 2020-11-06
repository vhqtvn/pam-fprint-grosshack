/*
 * /net/reactivated/Fprint/Manager object implementation
 * Copyright (C) 2008 Daniel Drake <dsd@gentoo.org>
 * Copyright (C) 2020 Marco Trevisan <marco.trevisan@canonical.com>
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

#include <unistd.h>
#include <stdlib.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <fprint.h>
#include <glib-object.h>

#include "fprintd.h"

static void fprint_manager_constructed (GObject *object);
static gboolean fprint_manager_get_devices(FprintManager *manager,
	GPtrArray **devices, GError **error);
static gboolean fprint_manager_get_default_device(FprintManager *manager,
	const char **device, GError **error);

typedef struct
{
	GDBusConnection *connection;
	GDBusObjectManager *object_manager;
	FprintDBusManager *dbus_manager;
	FpContext *context;
	gboolean no_timeout;
	guint timeout_id;
} FprintManagerPrivate;

G_DEFINE_TYPE_WITH_CODE(FprintManager, fprint_manager, G_TYPE_OBJECT, G_ADD_PRIVATE (FprintManager))

enum {
	PROP_0,
	FPRINT_MANAGER_CONNECTION,
	N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void fprint_manager_finalize(GObject *object)
{
	FprintManagerPrivate *priv = fprint_manager_get_instance_private (FPRINT_MANAGER (object));

	g_clear_object (&priv->object_manager);
	g_clear_object (&priv->dbus_manager);
	g_clear_object (&priv->connection);
	g_clear_object (&priv->context);

	G_OBJECT_CLASS(fprint_manager_parent_class)->finalize(object);
}

static FprintDevice *
fprint_dbus_object_skeleton_get_device (FprintDBusObjectSkeleton *object) {
	FprintDevice *rdev;

	g_object_get (object, "device", &rdev, NULL);
	return rdev;
}

static void fprint_manager_set_property (GObject *object, guint property_id,
					 const GValue *value, GParamSpec *pspec)
{
	FprintManager *self = FPRINT_MANAGER (object);
	FprintManagerPrivate *priv = fprint_manager_get_instance_private (self);

	switch (property_id) {
	case FPRINT_MANAGER_CONNECTION:
		priv->connection = g_value_dup_object (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
		break;
	}
}

static void fprint_manager_get_property (GObject *object, guint property_id,
					 GValue *value, GParamSpec *pspec)
{
	FprintManager *self = FPRINT_MANAGER (object);
	FprintManagerPrivate *priv = fprint_manager_get_instance_private (self);

	switch (property_id) {
	case FPRINT_MANAGER_CONNECTION:
		g_value_set_object (value, priv->connection);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
		break;
	}
}

static void fprint_manager_class_init(FprintManagerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->constructed = fprint_manager_constructed;
	object_class->set_property = fprint_manager_set_property;
	object_class->get_property = fprint_manager_get_property;
	object_class->finalize = fprint_manager_finalize;

	properties[FPRINT_MANAGER_CONNECTION] =
		g_param_spec_object ("connection",
				     "Connection",
				     "Set GDBus connection property",
				     G_TYPE_DBUS_CONNECTION,
				     G_PARAM_CONSTRUCT_ONLY |
				     G_PARAM_READWRITE);

	g_object_class_install_properties (object_class, N_PROPS, properties);
}

static gchar *get_device_path(FprintDevice *rdev)
{
	return g_strdup_printf (FPRINT_SERVICE_PATH "/Device/%d",
		_fprint_device_get_id(rdev));
}

static gboolean
fprint_manager_timeout_cb (FprintManager *manager)
{
	//FIXME kill all the devices
	exit(0);
	return FALSE;
}

static void
fprint_manager_in_use_notified (FprintDevice *rdev, GParamSpec *spec, FprintManager *manager)
{
	FprintManagerPrivate *priv = fprint_manager_get_instance_private (manager);
	guint num_devices_used = 0;
	g_autolist(GDBusObject) devices = NULL;
	GList *l;
	gboolean in_use;

	if (priv->timeout_id > 0) {
		g_source_remove (priv->timeout_id);
		priv->timeout_id = 0;
	}
	if (priv->no_timeout)
		return;

	devices = g_dbus_object_manager_get_objects (priv->object_manager);

	for (l = devices; l != NULL; l = l->next) {
		g_autoptr(FprintDevice) dev = NULL;
		FprintDBusObjectSkeleton *object = l->data;

		dev = fprint_dbus_object_skeleton_get_device (object);
		g_object_get (G_OBJECT(dev), "in-use", &in_use, NULL);
		if (in_use != FALSE)
			num_devices_used++;
	}

	if (num_devices_used == 0)
		priv->timeout_id = g_timeout_add_seconds (TIMEOUT, (GSourceFunc) fprint_manager_timeout_cb, manager);
}

static gboolean
handle_get_devices (FprintManager *manager, GDBusMethodInvocation *invocation,
		    FprintDBusManager *skeleton)
{
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GError) error = NULL;

	if (!fprint_manager_get_devices (manager, &devices, &error)) {
		g_dbus_method_invocation_return_gerror (invocation, error);
		return TRUE;
	}

	fprint_dbus_manager_complete_get_devices (skeleton, invocation,
						  (const gchar *const *)
						  devices->pdata);

	return TRUE;
}

static gboolean
handle_get_default_device (FprintManager *manager,
			   GDBusMethodInvocation *invocation,
			   FprintDBusManager *skeleton)
{
	const gchar *device;
	g_autoptr(GError) error = NULL;

	if (!fprint_manager_get_default_device (manager, &device, &error)) {
		g_dbus_method_invocation_return_gerror (invocation, error);
		return TRUE;
	}

	fprint_dbus_manager_complete_get_default_device (skeleton, invocation,
							 device);

	return TRUE;
}

static void
device_added_cb (FprintManager *manager, FpDevice *device, FpContext *context)
{
	FprintManagerPrivate *priv = fprint_manager_get_instance_private (manager);
	g_autoptr(FprintDBusObjectSkeleton) object = NULL;
	FprintDevice *rdev = fprint_device_new(device);
	g_autofree gchar *path = NULL;

	g_signal_connect (G_OBJECT(rdev), "notify::in-use",
			  G_CALLBACK (fprint_manager_in_use_notified), manager);

	path = get_device_path (rdev);

	object = fprint_dbus_object_skeleton_new (path);
	fprint_dbus_object_skeleton_set_device (object,
						FPRINT_DBUS_DEVICE (rdev));
	g_dbus_object_manager_server_export (
		G_DBUS_OBJECT_MANAGER_SERVER (priv->object_manager),
		G_DBUS_OBJECT_SKELETON (object));
}

static void
device_removed_cb (FprintManager *manager, FpDevice *device, FpContext *context)
{
	FprintManagerPrivate *priv = fprint_manager_get_instance_private (manager);
	g_autolist (FprintDBusObjectSkeleton) objects = NULL;
	GList *item;

	objects = g_dbus_object_manager_get_objects (priv->object_manager);

	for (item = objects; item; item = item->next) {
		g_autoptr(FprintDevice) rdev = NULL;
		g_autoptr(FpDevice) dev = NULL;
		FprintDBusObjectSkeleton *object = item->data;

		rdev = fprint_dbus_object_skeleton_get_device (object);
		g_object_get (rdev, "dev", &dev, NULL);
		if (dev != device)
			continue;

		g_dbus_interface_skeleton_unexport (
			G_DBUS_INTERFACE_SKELETON (rdev));

		g_signal_handlers_disconnect_by_data (rdev, manager);

		/* We cannot continue to iterate at this point, but we don't need to either */
		break;
	}

	/* The device that disappeared might have been in-use.
	 * Do we need to do anything else in this case to clean up more gracefully? */
	fprint_manager_in_use_notified (NULL, NULL, manager);
}

static void fprint_manager_constructed (GObject *object)
{
	FprintManager *manager = FPRINT_MANAGER (object);
	FprintManagerPrivate *priv = fprint_manager_get_instance_private (manager);
	GDBusObjectManagerServer *object_manager_server;

	object_manager_server =
		g_dbus_object_manager_server_new (FPRINT_SERVICE_PATH "/Device");

	priv->object_manager = G_DBUS_OBJECT_MANAGER (object_manager_server);
	priv->dbus_manager = fprint_dbus_manager_skeleton_new ();
	priv->context = fp_context_new ();

	g_signal_connect_object (priv->dbus_manager,
				 "handle-get-devices",
				 G_CALLBACK (handle_get_devices),
				 manager,
				 G_CONNECT_SWAPPED);
	g_signal_connect_object (priv->dbus_manager,
				 "handle-get-default-device",
				 G_CALLBACK (handle_get_default_device),
				 manager,
				 G_CONNECT_SWAPPED);

	g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (priv->dbus_manager),
					  priv->connection,
					  FPRINT_SERVICE_PATH "/Manager", NULL);

	g_dbus_object_manager_server_set_connection (object_manager_server,
						     priv->connection);

	/* And register the signals for initial enumeration and hotplug. */
	g_signal_connect_object (priv->context,
				 "device-added",
				 (GCallback) device_added_cb,
				 manager,
				 G_CONNECT_SWAPPED);

	g_signal_connect_object (priv->context,
				 "device-removed",
				 (GCallback) device_removed_cb,
				 manager,
				 G_CONNECT_SWAPPED);

	/* Prepare everything by enumerating all devices.
	 * This blocks the main loop until the existing devices are enumerated
	 */
	fp_context_enumerate (priv->context);

	G_OBJECT_CLASS (fprint_manager_parent_class)->constructed (object);
}

static void
fprint_manager_init (FprintManager *manager)
{
}

FprintManager *fprint_manager_new (GDBusConnection *connection, gboolean no_timeout)
{
	FprintManagerPrivate *priv;
	GObject *object;

	object = g_object_new (FPRINT_TYPE_MANAGER, "connection", connection, NULL);
	priv = fprint_manager_get_instance_private (FPRINT_MANAGER (object));
	priv->no_timeout = no_timeout;

	if (!priv->no_timeout)
		priv->timeout_id = g_timeout_add_seconds (TIMEOUT, (GSourceFunc) fprint_manager_timeout_cb, object);

	return FPRINT_MANAGER (object);
}

static gboolean fprint_manager_get_devices(FprintManager *manager,
	GPtrArray **devices, GError **error)
{
	FprintManagerPrivate *priv = fprint_manager_get_instance_private (manager);
	g_autolist (FprintDBusObjectSkeleton) objects = NULL;
	GList *l;
	int num_open;
	GPtrArray *devs;

	objects = g_dbus_object_manager_get_objects (priv->object_manager);
	objects = g_list_reverse (objects);

	num_open = g_list_length (objects);
	devs = g_ptr_array_sized_new(num_open);

	if (num_open > 0) {
		for (l = objects; l != NULL; l = l->next) {
			g_autoptr(FprintDevice) rdev = NULL;
			FprintDBusObjectSkeleton *object = l->data;
			const char *path;

			rdev = fprint_dbus_object_skeleton_get_device (object);
			path = g_dbus_interface_skeleton_get_object_path (
				G_DBUS_INTERFACE_SKELETON (rdev));
			g_ptr_array_add (devs, (char *) path);
		}
	}
	g_ptr_array_add (devs, NULL);

	*devices = devs;
	return TRUE;
}

static gboolean fprint_manager_get_default_device(FprintManager *manager,
	const char **device, GError **error)
{
	FprintManagerPrivate *priv = fprint_manager_get_instance_private (manager);
	g_autolist (FprintDBusObjectSkeleton) objects = NULL;
	int num_open;

	objects = g_dbus_object_manager_get_objects (priv->object_manager);
	num_open = g_list_length (objects);

	if (num_open > 0) {
		g_autoptr(FprintDevice) rdev = NULL;
		FprintDBusObjectSkeleton *object = g_list_last (objects)->data;

		rdev = fprint_dbus_object_skeleton_get_device (object);
		*device = g_dbus_interface_skeleton_get_object_path (
			G_DBUS_INTERFACE_SKELETON (rdev));
		return TRUE;
	} else {
		g_set_error (error, FPRINT_ERROR, FPRINT_ERROR_NO_SUCH_DEVICE,
			     "No devices available");
		*device = NULL;
		return FALSE;
	}
}

GQuark fprint_error_quark (void)
{
	static volatile gsize quark = 0;
	if (g_once_init_enter (&quark)) {
		g_autoptr(GEnumClass) errors_enum = NULL;
		GQuark domain;
		unsigned i;

		domain = g_quark_from_static_string ("fprintd-error-quark");
		errors_enum = g_type_class_ref (FPRINT_TYPE_ERROR);

		for (i = 0; i < errors_enum->n_values; ++i) {
			GEnumValue *value = &errors_enum->values[i];

			g_dbus_error_register_error (domain, value->value,
						     value->value_nick);
		}

		g_once_init_leave (&quark, domain);
	}
	return (GQuark) quark;
}
