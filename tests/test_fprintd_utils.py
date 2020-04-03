#!/usr/bin/python3

# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU Lesser General Public License as published by the Free
# Software Foundation; either version 3 of the License, or (at your option) any
# later version.  See http://www.gnu.org/copyleft/lgpl.html for the full text
# of the license.

__author__ = 'Bastien Nocera'
__email__ = 'hadess@hadess.net'
__copyright__ = '(c) 2020 Red Hat Inc.'
__license__ = 'LGPL 3+'

import tempfile
import unittest
import sys
import subprocess
import dbus
import dbus.mainloop.glib
import dbusmock
import fcntl
import os
import time


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

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)

class TestFprintdUtilsBase(dbusmock.DBusTestCase):
    '''Test fprintd utilities'''

    @classmethod
    def setUpClass(klass):
        klass.start_system_bus()
        klass.dbus_con = klass.get_dbus(True)
        klass.sleep_time = 0.5

        template_path = './'
        if 'TOPSRCDIR' in os.environ:
            template_path = os.environ['TOPSRCDIR'] + '/tests/'
        klass.template_name = template_path + 'dbusmock/fprintd.py'
        print ('Using template from %s' % klass.template_name)

        klass.tools_prefix = ''
        if 'FPRINT_BUILD_DIR' in os.environ:
            klass.tools_prefix = os.environ['FPRINT_BUILD_DIR'] + '/../utils/'
            print ('Using tools from %s' % klass.tools_prefix)
        else:
            print ('Using tools from $PATH')

        klass.wrapper_args = []
        klass.valgrind = False
        if 'VALGRIND' in os.environ:
            valgrind = os.environ['VALGRIND']
            if valgrind is not None:
                klass.valgrind = True
                klass.sleep_time *= 4
                klass.wrapper_args = ['valgrind', '--leak-check=full']
                if os.path.exists(valgrind):
                    klass.wrapper_args += ['--suppressions={}'.format(valgrind)]

        if 'ADDRESS_SANITIZER' in os.environ:
            klass.sleep_time *= 2

    def setUp(self):
        super().setUp()
        (self.p_mock, self.obj_fprintd_manager) = self.spawn_server_template(
            self.template_name, {}, stdout=subprocess.PIPE)
        # set log to nonblocking
        flags = fcntl.fcntl(self.p_mock.stdout, fcntl.F_GETFL)
        fcntl.fcntl(self.p_mock.stdout, fcntl.F_SETFL, flags | os.O_NONBLOCK)
        self.obj_fprintd_mock = dbus.Interface(self.obj_fprintd_manager, 'net.reactivated.Fprint.Manager.Mock')

    def tearDown(self):
        self.p_mock.terminate()
        self.p_mock.wait()
        super().tearDown()

    def setup_device(self):
        self.device_path = self.obj_fprintd_mock.AddDevice(
            'FDO Trigger Finger Laser Reader', 3, 'swipe')
        self.device_mock = self.dbus_con.get_object('net.reactivated.Fprint',
            self.device_path)
        self.set_enrolled_fingers(['left-little-finger', 'right-little-finger'])

    def set_enrolled_fingers(self, fingers, user='toto'):
        self.enrolled_fingers = fingers
        self.device_mock.SetEnrolledFingers('toto', self.enrolled_fingers,
            signature='sas')

    def start_utility_process(self, utility_name, args=[], sleep=True):
        utility = [ os.path.join(self.tools_prefix, 'fprintd-{}'.format(utility_name)) ]
        process = subprocess.Popen(self.wrapper_args + utility + args,
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT,
                                   universal_newlines=True)
        flags = fcntl.fcntl(process.stdout, fcntl.F_GETFL)
        fcntl.fcntl(process.stdout, fcntl.F_SETFL, flags | os.O_NONBLOCK)

        self.addCleanup(self.try_stop_utility_process, process)

        if sleep:
            time.sleep(self.sleep_time)

        return process

    def stop_utility_process(self, process):
        print(process.stdout.read())
        process.terminate()
        process.wait()

    def try_stop_utility_process(self, process):
        try:
            self.stop_utility_process(process)
        except:
            pass

    def get_process_output(self, process):
        out = process.stdout.read()
        self.addCleanup(print, out)
        return out

    def run_utility_process(self, utility_name, args=[], sleep=True, timeout=None):
        proc = self.start_utility_process(utility_name, args=args, sleep=sleep)
        ret = proc.wait(timeout=timeout if timeout is not None else self.sleep_time * 4)
        self.assertLessEqual(ret, 128)

        return self.get_process_output(proc), ret


class TestFprintdUtils(TestFprintdUtilsBase):
    def setUp(self):
        super().setUp()
        self.setup_device()

    def test_fprintd_enroll(self):
        process = self.start_utility_process('enroll', ['-f', 'right-index-finger', 'toto'])

        out = self.get_process_output(process)
        self.assertRegex(out, r'right-index-finger')

        self.device_mock.EmitEnrollStatus('enroll-completed', True)
        time.sleep(self.sleep_time)

        out = self.get_process_output(process)
        self.assertRegex(out, 'Enroll result: enroll-completed')

    def test_fprintd_list(self):
        # Rick has no fingerprints enrolled
        out, ret = self.run_utility_process('list', ['rick'])
        self.assertRegex(out, r'has no fingers enrolled for')
        self.assertEqual(ret, 0)

        # Toto does
        out, ret = self.run_utility_process('list', ['toto'])
        self.assertRegex(out, r'right-little-finger')
        self.assertEqual(ret, 0)

    def test_fprintd_delete(self):
        # Has fingerprints enrolled
        out, ret = self.run_utility_process('list', ['toto'])
        self.assertRegex(out, r'left-little-finger')
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'right-little-finger')

        # Delete fingerprints
        out, ret = self.run_utility_process('delete', ['toto'])
        self.assertRegex(out, r'Fingerprints deleted')
        self.assertEqual(ret, 0)

        # Doesn't have fingerprints
        out, ret = self.run_utility_process('list', ['toto'])
        self.assertRegex(out, r'has no fingers enrolled for')
        self.assertEqual(ret, 0)


class TestFprintdUtilsVerify(TestFprintdUtilsBase):
    def setUp(self):
        super().setUp()
        self.setup_device()

    def start_verify_process(self, user='toto', finger=None, checkEnrolled=True):
        args = [user]
        if finger:
            args += ['-f', finger]

        self.process = self.start_utility_process('verify', args)
        out = self.get_process_output(self.process)

        self.assertNotRegex(out, r'Device already in use by [A-z]+')
        self.assertNotIn('Verify result:', out)

        if checkEnrolled and finger:
            self.assertNotIn('''Finger '{}' not enrolled for user {}'''.format(
                finger, user), out)

        if checkEnrolled:
            for f in self.enrolled_fingers:
                self.assertIn(f, out)

        if finger:
            expected_finger = finger
            if finger == 'any' and not self.device_mock.HasIdentification():
                expected_finger = self.enrolled_fingers[0]
            self.assertEqual(self.device_mock.GetSelectedFinger(), expected_finger)

    def assertVerifyMatch(self, match):
        self.assertIn('Verify result: {} (done)'.format(
            'verify-match' if match else 'verify-no-match'),
            self.get_process_output(self.process))

    def test_fprintd_verify(self):
        self.start_verify_process()

        self.device_mock.EmitVerifyStatus('verify-match', True)
        time.sleep(self.sleep_time)
        self.assertVerifyMatch(True)

    def test_fprintd_verify_enrolled_fingers(self):
        for finger in self.enrolled_fingers:
            self.start_verify_process(finger=finger)

            self.device_mock.EmitVerifyStatus('verify-match', True)
            time.sleep(self.sleep_time)
            self.assertVerifyMatch(True)

    def test_fprintd_verify_any_finger_no_identification(self):
        self.start_verify_process(finger='any')

        self.device_mock.EmitVerifyStatus('verify-match', True)
        time.sleep(self.sleep_time)
        self.assertVerifyMatch(True)

    def test_fprintd_verify_any_finger_identification(self):
        self.obj_fprintd_mock.RemoveDevice(self.device_path)
        self.device_path = self.obj_fprintd_mock.AddDevice('Full powered device',
            3, 'press', True)
        self.device_mock = self.dbus_con.get_object('net.reactivated.Fprint',
            self.device_path)
        self.set_enrolled_fingers(VALID_FINGER_NAMES)
        self.start_verify_process(finger='any')

        self.device_mock.EmitVerifyStatus('verify-match', True)
        time.sleep(self.sleep_time)
        self.assertVerifyMatch(True)

    def test_fprintd_verify_not_enrolled_fingers(self):
        for finger in [f for f in VALID_FINGER_NAMES if f not in self.enrolled_fingers]:
            regex = r'Finger \'{}\' not enrolled'.format(finger)
            with self.assertRaisesRegex(AssertionError, regex):
                self.start_verify_process(finger=finger)
            self.device_mock.Release()

    def test_fprintd_verify_no_enrolled_fingers(self):
        self.set_enrolled_fingers([])
        self.start_verify_process()
        self.assertEqual(self.process.poll(), 1)

    def test_fprintd_list_all_fingers(self):
        self.set_enrolled_fingers(VALID_FINGER_NAMES)
        self.start_verify_process()

    def test_fprintd_verify_script(self):
        script = [
            ( 'verify-match', True, 2 )
        ]
        self.device_mock.SetVerifyScript(script)

        self.start_verify_process()
        time.sleep(self.sleep_time * 4)
        self.assertVerifyMatch(True)

    def test_fprintd_multiple_verify_fails(self):
        self.start_verify_process()

        with self.assertRaisesRegex(AssertionError, r'Device already in use'):
            self.start_verify_process()

if __name__ == '__main__':
    # avoid writing to stderr
    unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout, verbosity=2))
