# -*- coding: utf-8 -*-

'''polkit mock template

This creates the basic methods and properties of the
org.freedesktop.PolicyKit1.Authority object, so that we can use it async
'''

# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU Lesser General Public License as published by the Free
# Software Foundation; either version 3 of the License, or (at your option) any
# later version.  See http://www.gnu.org/copyleft/lgpl.html for the full text
# of the license.

__author__ = 'Marco Trevisan'
__email__ = 'marco.trevisan@canonical.com'
__copyright__ = '(c) 2020 Canonical Ltd.'
__license__ = 'LGPL 3+'

import dbus

from dbusmock import MOCK_IFACE, mockobject

BUS_NAME = 'org.freedesktop.PolicyKit1'
MAIN_OBJ = '/org/freedesktop/PolicyKit1/Authority'
MAIN_IFACE = 'org.freedesktop.PolicyKit1.Authority'
SYSTEM_BUS = True
IS_OBJECT_MANAGER = False

def load(mock, parameters):
    polkitd = mockobject.objects[MAIN_OBJ]
    # default state
    polkitd.allow_unknown = False
    polkitd.allowed = []

    mock.AddProperties(MAIN_IFACE,
                       dbus.Dictionary({
                           'BackendName': 'local',
                           'BackendVersion': '0.8.15',
                           'BackendFeatures': dbus.UInt32(1, variant_level=1),
                       }, signature='sv'))


@dbus.service.method(MAIN_IFACE,
                     in_signature='(sa{sv})sa{ss}us', out_signature='(bba{ss})')
def CheckAuthorization(self, subject, action_id, details, flags, cancellation_id):
    return (action_id in self.allowed or self.allow_unknown, False, {'test': 'test'})


@dbus.service.method(MOCK_IFACE, in_signature='b', out_signature='')
def AllowUnknown(self, default):
    '''Control whether unknown actions are allowed

    This controls the return value of CheckAuthorization for actions which were
    not explicitly allowed by SetAllowed().
    '''
    self.allow_unknown = default


@dbus.service.method(MOCK_IFACE, in_signature='as', out_signature='')
def SetAllowed(self, actions):
    '''Set allowed actions'''

    self.allowed = actions

@dbus.service.method(MAIN_IFACE,
                     in_signature='', out_signature='o')
def GetDefaultDevice(self):
    devices = self.GetDevices()
    if len(devices) < 1:
        raise dbus.exceptions.DBusException(
            'No devices available',
            name='net.reactivated.Fprint.Error.NoSuchDevice')
    return devices[0]
