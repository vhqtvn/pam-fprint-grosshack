/*
 * fprintd header file
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

#pragma once

#include <glib.h>
#include <gio/gio.h>
#include <fprint.h>
#include "fprintd-dbus.h"

/* General */
#define TIMEOUT 30
#define FPRINT_SERVICE_NAME "net.reactivated.Fprint"
#define FPRINT_SERVICE_PATH "/net/reactivated/Fprint"

/* Errors */
GQuark fprint_error_quark(void);

#define FPRINT_ERROR fprint_error_quark()
#define FPRINT_ERROR_DBUS_INTERFACE "net.reactivated.Fprint.Error"
typedef enum {
	FPRINT_ERROR_CLAIM_DEVICE, /* developer didn't claim the device */
	FPRINT_ERROR_ALREADY_IN_USE, /* device is already claimed by somebody else */
	FPRINT_ERROR_INTERNAL, /* internal error occurred */
	FPRINT_ERROR_PERMISSION_DENIED, /* PolicyKit refused the action */
	FPRINT_ERROR_NO_ENROLLED_PRINTS, /* No prints are enrolled */
	FPRINT_ERROR_NO_ACTION_IN_PROGRESS, /* No actions currently in progress */
	FPRINT_ERROR_INVALID_FINGERNAME, /* the finger name passed was invalid */
	FPRINT_ERROR_NO_SUCH_DEVICE, /* device does not exist */
} FprintError;

typedef enum {
	FPRINT_DEVICE_PERMISSION_NONE = 0,
	FPRINT_DEVICE_PERMISSION_ENROLL = (1 << 0), /*< nick=net.reactivated.fprint.device.enroll >*/
	FPRINT_DEVICE_PERMISSION_SETUSERNAME = (1 << 1), /*< nick=net.reactivated.fprint.device.setusername >*/
	FPRINT_DEVICE_PERMISSION_VERIFY = (1 << 2), /*< nick=net.reactivated.fprint.device.verify >*/
} FprintDevicePermission;

/* Manager */
#define FPRINT_TYPE_MANAGER            (fprint_manager_get_type())
G_DECLARE_FINAL_TYPE (FprintManager, fprint_manager, FPRINT, MANAGER, GObject)

struct _FprintManager {
	GObject parent;
};

FprintManager *fprint_manager_new (GDBusConnection *connection, gboolean no_timeout);

/* Device */
#define FPRINT_TYPE_DEVICE            (fprint_device_get_type())
G_DECLARE_FINAL_TYPE (FprintDevice, fprint_device, FPRINT, DEVICE,
		      FprintDBusDeviceSkeleton)

struct _FprintDevice {
	FprintDBusDeviceSkeleton parent;
};

FprintDevice *fprint_device_new(FpDevice *dev);
guint32 _fprint_device_get_id(FprintDevice *rdev);
/* Print */
/* TODO */
