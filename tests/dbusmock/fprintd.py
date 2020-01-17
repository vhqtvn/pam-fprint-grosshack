# -*- coding: utf-8 -*-

'''fprintd mock template

This creates the expected methods and properties of the
net.reactivated.Fprint.Manager object (/net/reactivated/Fprint/Manager)
but no devices.
'''

# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU Lesser General Public License as published by the Free
# Software Foundation; either version 3 of the License, or (at your option) any
# later version.  See http://www.gnu.org/copyleft/lgpl.html for the full text
# of the license.

__author__ = 'Bastien Nocera'
__email__ = 'hadess@hadess.net'
__copyright__ = '(c) 2020 Red Hat Inc.'
__license__ = 'LGPL 3+'

import dbus

from dbusmock import MOCK_IFACE, mockobject

BUS_NAME = 'net.reactivated.Fprint'
MAIN_OBJ = '/net/reactivated/Fprint/Manager'
SYSTEM_BUS = True
IS_OBJECT_MANAGER = False

MAIN_IFACE = 'net.reactivated.Fprint.Manager'
MANAGER_MOCK_IFACE = 'net.reactivated.Fprint.Manager.Mock'

DEVICE_IFACE = 'net.reactivated.Fprint.Device'
DEVICE_MOCK_IFACE = 'net.reactivated.Fprint.Device.Mock'

VALID_FINGER_NAMES = [
    'left-thumb',
    'left-index-finger',
    'left-middle-finger',
    'left-ring-finger',
    'left-little-finger',
    'right-thumb',
    'right-index-finger',
    'right-middle-finger',
    'right-ring-finger',
    'right-little-finger'
]

VALID_VERIFY_STATUS = [
    'verify-no-match',
    'verify-match',
    'verify-retry-scan',
    'verify-swipe-too-short',
    'verify-finger-not-centered',
    'verify-remove-and-retry',
    'verify-disconnected',
    'verify-unknown-error'
]

VALID_ENROLL_STATUS = [
    'enroll-completed',
    'enroll-failed',
    'enroll-stage-passed',
    'enroll-retry-scan',
    'enroll-swipe-too-short',
    'enroll-finger-not-centered',
    'enroll-remove-and-retry',
    'enroll-data-full',
    'enroll-disconnected',
    'enroll-unknown-error'
]

# Ever incrementing device ID
last_id = 0

def load(mock, parameters):
    fprintd = mockobject.objects[MAIN_OBJ]
    mock.last_device_id = 0
    fprintd.fingers = {}

@dbus.service.method(MAIN_IFACE,
                     in_signature='', out_signature='ao')
def GetDevices(self):
    return [(k) for k in mockobject.objects.keys() if "/Device/" in k]

@dbus.service.method(MAIN_IFACE,
                     in_signature='', out_signature='o')
def GetDefaultDevice(self):
    devices = self.GetDevices()
    if len(devices) < 1:
        raise dbus.exceptions.DBusException(
            'No devices available',
            name='net.reactivated.Fprint.Error.NoSuchDevice')
    return devices[0]

@dbus.service.method(MANAGER_MOCK_IFACE,
                     in_signature='sis', out_signature='s')
def AddDevice(self, device_name, num_enroll_stages, scan_type):
    '''Convenience method to add a fingerprint reader device

    You have to specify a device name, the number of enrollment
    stages it would use (> 0) and the scan type, as a string
    (either 'press' or 'swipe')
    '''

    if scan_type not in ['swipe', 'press']:
        raise dbus.exceptions.DBusException(
            'Invalid scan_type \'%s\'.' % scan_type,
            name='org.freedesktop.DBus.Error.InvalidArgs')

    if num_enroll_stages <= 0:
        raise dbus.exceptions.DBusException(
            'Invalid num_enroll_stages \'%s\'.' % num_enroll_stages,
            name='org.freedesktop.DBus.Error.InvalidArgs')

    self.last_device_id += 1
    path = '/net/reactivated/Fprint/Device/%d' % last_id
    device_properties = {
        'name': dbus.String(device_name, variant_level=1),
        'num-enroll-stages': dbus.UInt32(num_enroll_stages, variant_level=1),
        'scan-type': scan_type
    }

    self.AddObject(path,
                   DEVICE_IFACE,
                   # Properties
                   device_properties,
                   # Methods
                   [
                       ('ListEnrolledFingers', 's', 'as', ListEnrolledFingers),
                       ('DeleteEnrolledFingers', 's', '', DeleteEnrolledFingers),
                       ('DeleteEnrolledFingers2', '', '', DeleteEnrolledFingers2),
                       ('Claim', 's', '', Claim),
                       ('Release', '', '', Release),
                       ('VerifyStart', 's', '', VerifyStart),
                       ('VerifyStop', '', '', VerifyStop),
                       ('EnrollStart', 's', '', EnrollStart),
                       ('EnrollStop', '', '', EnrollStop)
    ])

    device = mockobject.objects[path]
    device.fingers = {}
    device.claimed_user = None
    device.action = None

    return path

@dbus.service.method(DEVICE_IFACE,
                     in_signature='s', out_signature='as')
def ListEnrolledFingers(device, user):
    if user in device.fingers:
        return device.fingers[user]
    raise dbus.exceptions.DBusException(
        'No enrolled prints in device %s for user %s' % (device.path, user),
        name='net.reactivated.Fprint.Error.NoEnrolledPrints')

@dbus.service.method(DEVICE_IFACE,
                     in_signature='s', out_signature='')
def DeleteEnrolledFingers(device, user):
    device.fingers[user] = []

@dbus.service.method(DEVICE_IFACE,
                     in_signature='', out_signature='')
def DeleteEnrolledFingers2(device):
    if not device.claimed_user:
        raise dbus.exceptions.DBusException(
            'Device was not claimed before use',
            name='net.reactivated.Fprint.Error.ClaimDevice')
    device.fingers[device.claimed_user] = []

@dbus.service.method(DEVICE_IFACE,
                     in_signature='s', out_signature='')
def Claim(device, user):
    if device.claimed_user:
        raise dbus.exceptions.DBusException(
            'Device already in use by %s' % device.claimed_user,
            name='net.reactivated.Fprint.Error.AlreadyInUse')

    device.claimed_user = user

@dbus.service.method(DEVICE_IFACE,
                     in_signature='', out_signature='')
def Release(device):
    if not device.claimed_user:
        raise dbus.exceptions.DBusException(
            'Device was not claimed before use',
            name='net.reactivated.Fprint.Error.ClaimDevice')
    device.claimed_user = None

def can_verify_finger(device, finger_name):
    # We should already have checked that there are enrolled fingers
    if finger_name == 'any':
        return True
    if finger_name in device.fingers[device.claimed_user]:
        return True
    return False

@dbus.service.method(DEVICE_IFACE,
                     in_signature='s', out_signature='')
def VerifyStart(device, finger_name):
    if not device.claimed_user:
        raise dbus.exceptions.DBusException(
            'Device was not claimed before use',
            name='net.reactivated.Fprint.Error.ClaimDevice')
    if device.claimed_user not in device.fingers:
        raise dbus.exceptions.DBusException(
            'No enrolled prints for user \'%s\'' % device.claimed_user,
            name='net.reactivated.Fprint.Error.NoEnrolledPrints')
    if not finger_name:
        raise dbus.exceptions.DBusException(
            'Invalid empty finger_name.',
            name='org.freedesktop.DBus.Error.InvalidArgs')
    if not can_verify_finger(device, finger_name):
        raise dbus.exceptions.DBusException(
            'Finger \'%s\' not enrolled.' % finger_name,
            name='org.freedesktop.DBus.Error.Internal')
    if device.action:
        raise dbus.exceptions.DBusException(
            'Action \'%s\' already in progress' % device.action,
            name='net.reactivated.Fprint.Error.AlreadyInUse')
    device.action = 'verify'

    if finger_name == 'any':
        finger_name = device.fingers[device.claimed_user][0]
    device.EmitSignal(DEVICE_IFACE, 'VerifyFingerSelected', 's', [
                          finger_name
                      ])

@dbus.service.method(DEVICE_MOCK_IFACE,
                     in_signature='sb', out_signature='')
def EmitVerifyStatus(device, result, done):
    if (not device.action) or (device.action != 'verify'):
        raise dbus.exceptions.DBusException(
            'Cannot send verify statuses when not verifying',
            name='org.freedesktop.DBus.Error.InvalidArgs')
    if result not in VALID_VERIFY_STATUS:
        raise dbus.exceptions.DBusException(
            'Unknown verify status \'%s\'' % result,
            name='org.freedesktop.DBus.Error.InvalidArgs')
    device.EmitSignal(DEVICE_IFACE, 'VerifyStatus', 'sb', [
                          result,
                          done
                      ])

@dbus.service.method(DEVICE_IFACE,
                     in_signature='', out_signature='')
def VerifyStop(device):
    if device.action != 'verify':
        raise dbus.exceptions.DBusException(
            'No verification to stop',
            name='net.reactivated.Fprint.Error.NoActionInProgress')
    device.action = None

@dbus.service.method(DEVICE_IFACE,
                     in_signature='s', out_signature='')
def EnrollStart(device, finger_name):
    if finger_name not in VALID_FINGER_NAMES:
        raise dbus.exceptions.DBusException(
            'Invalid finger name \'%s\'' % finger_name,
            name='net.reactivated.Fprint.Error.InvalidFingername')
    if not device.claimed_user:
        raise dbus.exceptions.DBusException(
            'Device was not claimed before use',
            name='net.reactivated.Fprint.Error.ClaimDevice')
    if device.action:
        raise dbus.exceptions.DBusException(
            'Action \'%s\' already in progress' % device.action,
            name='net.reactivated.Fprint.Error.AlreadyInUse')
    device.action = 'enroll'

@dbus.service.method(DEVICE_MOCK_IFACE,
                     in_signature='sb', out_signature='')
def EmitEnrollStatus(device, result, done):
    if (not device.action) or (device.action != 'enroll'):
        raise dbus.exceptions.DBusException(
            'Cannot send enroll statuses when not enrolling',
            name='org.freedesktop.DBus.Error.InvalidArgs')
    if result not in VALID_ENROLL_STATUS:
        raise dbus.exceptions.DBusException(
            'Unknown enroll status \'%s\'' % result,
            name='org.freedesktop.DBus.Error.InvalidArgs')
    device.EmitSignal(DEVICE_IFACE, 'EnrollStatus', 'sb', [
                          result,
                          done
                      ])
    # FIXME save enrolled finger?

@dbus.service.method(DEVICE_IFACE,
                     in_signature='', out_signature='')
def EnrollStop(device):
    if device.action != 'enroll':
        raise dbus.exceptions.DBusException(
            'No enrollment to stop',
            name='net.reactivated.Fprint.Error.NoActionInProgress')
    device.action = None

@dbus.service.method(DEVICE_MOCK_IFACE,
                     in_signature='sas', out_signature='')
def SetEnrolledFingers(device, user, fingers):
    '''Convenience method to set the list of enrolled fingers.

    The device_path is the return value from AddDevice(), and the
    array of fingers must only contain valid finger names.

    Returns nothing.
    '''

    if len(fingers) < 1:
        raise dbus.exceptions.DBusException(
            'Fingers array must not be empty',
            name='org.freedesktop.DBus.Error.InvalidArgs')

    for k in fingers:
        if k not in VALID_FINGER_NAMES:
            raise dbus.exceptions.DBusException(
                'Invalid finger name \'%s\'' % k,
                name='org.freedesktop.DBus.Error.InvalidArgs')

    device.fingers[user] = fingers

