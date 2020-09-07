xf86-input-emulated - a driver for Xorg XFree86 input event emulation
=====================================================================

The official repository for this driver is
https://gitlab.freedesktop.org/xorg/driver/xf86-input-emulated

***WARNING: misconfiguration of an X input driver may leave you without
usable input devices in your X session. Use with caution.***

Prerequisites
-------------

To build, you'll need the X.Org X server SDK (check your distribution for a
xorg-x11-server-devel package or similar).

To build the X server from source:
https://www.x.org/wiki/Building_the_X_Window_System/

Building
--------

To build this driver:

    autoreconf -vif
    ./configure --prefix=$HOME/build
    make && make install

Note that this assumes the same prefix as used in "Building the X Window
System" above, adjust as required. If you want a system install, use a
prefix of */usr*.

Bugs
----

Bugs in this driver go to the Issues section of its gitlab project:
https://gitlab.freedesktop.org/xorg/driver/xf86-input-emulated/issues
