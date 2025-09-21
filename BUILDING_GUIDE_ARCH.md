# Building pam-fprint-grosshack on Arch Linux

This guide will walk you through the process of building the pam-fprint-grosshack package from source on Arch Linux.

## Overview

pam-fprint-grosshack is a fork of the pam module which implements the simultaneous password and fingerprint behavior. It allows authentication with either fingerprint or password without requiring users to wait for fingerprint authentication to fail before entering a password.

## Prerequisites

Install the required dependencies:

```bash
# Runtime dependencies
sudo pacman -S glib2 polkit dbus dbus-glib systemd

# Build dependencies
sudo pacman -S base-devel git cmake meson pam_wrapper python-cairo python-dbus python-dbusmock libfprint pam
```

Note: The build dependencies (`makedepends`) can be safely removed after the compilation is complete as they are only needed during the build process.

## Building from Source

1. Clone the repository (if you haven't already):

```bash
git clone https://github.com/vhqtvn/pam-fprint-grosshack.git
cd pam-fprint-grosshack
```

2. Configure the build with meson:

```bash
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig meson setup builddir --wipe --prefix=/usr/local --bindir=/usr/local/bin
```

3. Compile the project:

```bash
meson compile -C builddir
```

4. Install the module (requires root privileges):

```bash
sudo meson install -C builddir
```

## Configuration

To use this module, add the following lines to the appropriate files in `/etc/pam.d/` (e.g., `/etc/pam.d/system-auth`):

```
auth    sufficient    pam_fprintd_grosshack.so
auth    sufficient    pam_unix.so try_first_pass nullok
```

## Verifying Installation

To verify if the module is correctly installed and functioning:

1. Check if the module file exists:

```bash
ls -l /usr/lib/security/pam_fprintd_grosshack.so
```

2. Ensure fprintd service is running:

```bash
systemctl status fprintd.service
```

3. If not running, start it:

```bash
sudo systemctl start fprintd.service
sudo systemctl enable fprintd.service
```

4. Enroll your fingerprint:

```bash
fprintd-enroll
```

5. Test the authentication:

```bash
fprintd-verify
```

## Troubleshooting

If you encounter issues:

1. Check fprintd logs:

```bash
journalctl -u fprintd.service
```

2. Enable debug output for fprintd:

```bash
sudo systemctl edit fprintd.service
```

Add the following:

```
[Service]
Environment=G_MESSAGES_DEBUG=all
```

Then restart the service:

```bash
sudo systemctl restart fprintd.service
```

3. Verify your fingerprint reader is detected:

```bash
fprintd-list
```

## Warning

As noted in the README, this implementation was called a "gross hack" by the fprintd developers for a reason. Use with caution as it may have security flaws or other unforeseen bugs. 