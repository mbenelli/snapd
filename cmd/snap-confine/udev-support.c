/*
 * Copyright (C) 2015-2020 Canonical Ltd
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>

#include <libudev.h>

#include "../libsnap-confine-private/cleanup-funcs.h"
#include "../libsnap-confine-private/snap.h"
#include "../libsnap-confine-private/string-utils.h"
#include "../libsnap-confine-private/cgroup-support.h"
#include "../libsnap-confine-private/utils.h"
#include "udev-support.h"

__attribute__((format(printf, 2, 3)))
static void sc_dprintf(int fd, const char *format, ...);

typedef struct sc_cgroup_fds {
	int devices_allow_fd;
	int devices_deny_fd;
	int cgroup_procs_fd;
} sc_cgroup_fds;

static sc_cgroup_fds sc_cgroup_fds_new(void)
{
	sc_cgroup_fds empty = { -1, -1, -1 };
	return empty;
}

typedef struct sc_udev_device_group {
	bool is_v2;
	char *security_tag;
	union {
		struct {
			sc_cgroup_fds fds;
		} v1;
	};
} sc_device_cgroup;

static sc_cgroup_fds sc_udev_open_cgroup_v1(const char *security_tag);
static void sc_cleanup_cgroup_fds(sc_cgroup_fds * fds);

static void _sc_cgroup_v1_init(sc_device_cgroup * self)
{
	self->v1.fds = sc_cgroup_fds_new();

	/* initialize to something sane */
	sc_cgroup_fds fds = sc_udev_open_cgroup_v1(self->security_tag);
	if (fds.cgroup_procs_fd < 0) {
		die("cannot prepare cgroup v1 device hierarchy");
	}
	self->v1.fds = fds;

	/* Deny device access by default.
	 *
	 * Write 'a' to devices.deny to remove all existing devices that were added
	 * in previous launcher invocations, then add the static and assigned
	 * devices. This ensures that at application launch the cgroup only has
	 * what is currently assigned. */
	sc_dprintf(fds.devices_deny_fd, "a");
}

static void _sc_cgroup_v1_close(sc_device_cgroup * self)
{
	sc_cleanup_cgroup_fds(&self->v1.fds);
}

/**
 * SC_MINOR_ANY is used to indicate any minor device.
 */
const int SC_MINOR_ANY = INT_MAX;

static void _sc_cgroup_v1_allow(sc_device_cgroup * self, int kind, int major,
				int minor)
{
	if (minor != SC_MINOR_ANY) {
		sc_dprintf(self->v1.fds.devices_allow_fd,
			   "%c %u:%u rwm\n",
			   (kind == S_IFCHR) ? 'c' : 'b', major, minor);
	} else {
		/* use a mask to allow all minor devices for that major */
		sc_dprintf(self->v1.fds.devices_allow_fd,
			   "%c %u:* rwm\n",
			   (kind == S_IFCHR) ? 'c' : 'b', major);
	}
}

static void _sc_cgroup_v1_attach_pid(sc_device_cgroup * self, pid_t pid)
{
	sc_dprintf(self->v1.fds.cgroup_procs_fd, "%i\n", getpid());
}

static sc_device_cgroup *sc_device_cgroup_new(const char *security_tag)
{
	sc_device_cgroup *self = calloc(1, sizeof(sc_device_cgroup));
	if (self == NULL) {
		die("cannot allocate device cgroup wrapper");
	}
	self->is_v2 = sc_cgroup_is_v2();
	self->security_tag = sc_strdup(security_tag);

	if (!self->is_v2) {
		_sc_cgroup_v1_init(self);
	}

	return self;
}

static void sc_device_cgroup_close(sc_device_cgroup * self)
{
	if (!self->is_v2) {
		_sc_cgroup_v1_close(self);
	}
	sc_cleanup_string(&self->security_tag);
	free(self);
}

static void sc_device_cgroup_cleanup(sc_device_cgroup ** self)
{
	if (*self == NULL) {
		return;
	}
	sc_device_cgroup_close(*self);
	*self = NULL;
}

/**
 * sc_device_cgroup_allow sets up the cgroup to allow access to a given device
 * or a set of devices if SC_MINOR_ANY is passed as the minor number. The kind
 * must be one of S_IFCHR, S_IFBLK.
 */
static int sc_device_cgroup_allow(sc_device_cgroup * self, int kind, int major,
				  int minor)
{
	if (kind != S_IFCHR && kind != S_IFBLK) {
		die("unsupported device kind 0x%04x", kind);
	}
	if (!self->is_v2) {
		_sc_cgroup_v1_allow(self, kind, major, minor);
	}
	return 0;
}

static int sc_device_cgroup_attach_pid(sc_device_cgroup * self, pid_t pid)
{
	if (!self->is_v2) {
		_sc_cgroup_v1_attach_pid(self, pid);
	}
	return 0;
}

static void sc_dprintf(int fd, const char *format, ...)
{
	va_list ap1;
	va_list ap2;
	int n_expected, n_actual;

	va_start(ap1, format);
	va_copy(ap2, ap1);
	n_expected = vsnprintf(NULL, 0, format, ap2);
	n_actual = vdprintf(fd, format, ap1);
	if (n_actual == -1 || n_expected != n_actual) {
		die("cannot write to fd %d", fd);
	}
	va_end(ap2);
	va_end(ap1);
}

/* Allow access to common devices. */
static void sc_udev_allow_common(sc_device_cgroup * cgroup)
{
	/* The devices we add here have static number allocation.
	 * https://www.kernel.org/doc/html/v4.11/admin-guide/devices.html */
	sc_device_cgroup_allow(cgroup, S_IFCHR, 1, 3);	// /dev/null
	sc_device_cgroup_allow(cgroup, S_IFCHR, 1, 5);	// /dev/zero
	sc_device_cgroup_allow(cgroup, S_IFCHR, 1, 7);	// /dev/full
	sc_device_cgroup_allow(cgroup, S_IFCHR, 1, 8);	// /dev/random
	sc_device_cgroup_allow(cgroup, S_IFCHR, 1, 9);	// /dev/urandom
	sc_device_cgroup_allow(cgroup, S_IFCHR, 5, 0);	// /dev/tty
	sc_device_cgroup_allow(cgroup, S_IFCHR, 5, 1);	// /dev/console
	sc_device_cgroup_allow(cgroup, S_IFCHR, 5, 2);	// /dev/ptmx
}

/** Allow access to current and future PTY slaves.
 *
 * We unconditionally add them since we use a devpts newinstance. Unix98 PTY
 * slaves major are 136-143.
 *
 * See also:
 * https://www.kernel.org/doc/Documentation/admin-guide/devices.txt
 **/
static void sc_udev_allow_pty_slaves(sc_device_cgroup * cgroup)
{
	for (unsigned pty_major = 136; pty_major <= 143; pty_major++) {
		sc_device_cgroup_allow(cgroup, S_IFCHR, pty_major,
				       SC_MINOR_ANY);
	}
}

/** Allow access to Nvidia devices.
 *
 * Nvidia modules are proprietary and therefore aren't in sysfs and can't be
 * udev tagged. For now, just add existing nvidia devices to the cgroup
 * unconditionally (AppArmor will still mediate the access).  We'll want to
 * rethink this if snapd needs to mediate access to other proprietary devices.
 *
 * Device major and minor numbers are described in (though nvidia-uvm currently
 * isn't listed):
 *
 * https://www.kernel.org/doc/Documentation/admin-guide/devices.txt
 **/
static void sc_udev_allow_nvidia(sc_device_cgroup * cgroup)
{
	struct stat sbuf;

	/* Allow access to /dev/nvidia0 through /dev/nvidia254 */
	for (unsigned nv_minor = 0; nv_minor < 255; nv_minor++) {
		char nv_path[15] = { 0 };	// /dev/nvidiaXXX
		sc_must_snprintf(nv_path, sizeof(nv_path), "/dev/nvidia%u",
				 nv_minor);

		/* Stop trying to find devices after one is not found. In this manner,
		 * we'll add /dev/nvidia0 and /dev/nvidia1 but stop trying to find
		 * nvidia3 - nvidia254 if nvidia2 is not found. */
		if (stat(nv_path, &sbuf) < 0) {
			break;
		}
		sc_device_cgroup_allow(cgroup, S_IFCHR, major(sbuf.st_rdev),
				       minor(sbuf.st_rdev));
	}

	if (stat("/dev/nvidiactl", &sbuf) == 0) {
		sc_device_cgroup_allow(cgroup, S_IFCHR, major(sbuf.st_rdev),
				       minor(sbuf.st_rdev));
	}
	if (stat("/dev/nvidia-uvm", &sbuf) == 0) {
		sc_device_cgroup_allow(cgroup, S_IFCHR, major(sbuf.st_rdev),
				       minor(sbuf.st_rdev));
	}
	if (stat("/dev/nvidia-modeset", &sbuf) == 0) {
		sc_device_cgroup_allow(cgroup, S_IFCHR, major(sbuf.st_rdev),
				       minor(sbuf.st_rdev));
	}
}

/**
 * Allow access to /dev/uhid.
 *
 * Currently /dev/uhid isn't represented in sysfs, so add it to the device
 * cgroup if it exists and let AppArmor handle the mediation.
 **/
static void sc_udev_allow_uhid(sc_device_cgroup * cgroup)
{
	struct stat sbuf;

	if (stat("/dev/uhid", &sbuf) == 0) {
		sc_device_cgroup_allow(cgroup, S_IFCHR, major(sbuf.st_rdev),
				       minor(sbuf.st_rdev));
	}
}

/**
 * Allow access to /dev/net/tun
 *
 * When CONFIG_TUN=m, /dev/net/tun will exist but using it doesn't
 * autoload the tun module but also /dev/net/tun isn't udev tagged
 * until it is loaded. To work around this, if /dev/net/tun exists, add
 * it unconditionally to the cgroup and rely on AppArmor to mediate the
 * access. LP: #1859084
 **/
static void sc_udev_allow_dev_net_tun(sc_device_cgroup * cgroup)
{
	struct stat sbuf;

	if (stat("/dev/net/tun", &sbuf) == 0) {
		sc_device_cgroup_allow(cgroup, S_IFCHR, major(sbuf.st_rdev),
				       minor(sbuf.st_rdev));
	}
}

/**
 * Allow access to assigned devices.
 *
 * The snapd udev security backend uses udev rules to tag matching devices with
 * tags corresponding to snap applications. Here we interrogate udev and allow
 * access to all assigned devices.
 **/
static void sc_udev_allow_assigned_device(sc_device_cgroup * cgroup,
					  struct udev_device *device)
{
	const char *path = udev_device_get_syspath(device);
	dev_t devnum = udev_device_get_devnum(device);
	unsigned int major = major(devnum);
	unsigned int minor = minor(devnum);
	/* The manual page of udev_device_get_devnum says:
	 * > On success, udev_device_get_devnum() returns the device type of
	 * > the passed device. On failure, a device type with minor and major
	 * > number set to 0 is returned. */
	if (major == 0 && minor == 0) {
		debug("cannot get major/minor numbers for syspath %s", path);
		return;
	}
	/* devnode is bound to the lifetime of the device and we cannot release
	 * it separately. */
	const char *devnode = udev_device_get_devnode(device);
	if (devnode == NULL) {
		debug("cannot find /dev node from udev device");
		return;
	}
	debug("inspecting type of device: %s", devnode);
	struct stat file_info;
	if (stat(devnode, &file_info) < 0) {
		debug("cannot stat %s", devnode);
		return;
	}
	int devtype = file_info.st_mode & S_IFMT;
	if (devtype == S_IFBLK || devtype == S_IFCHR) {
		sc_device_cgroup_allow(cgroup, devtype, major, minor);
	}
}

static void sc_udev_setup_acls_common(sc_device_cgroup * cgroup)
{

	/* Allow access to various devices. */
	sc_udev_allow_common(cgroup);
	sc_udev_allow_pty_slaves(cgroup);
	sc_udev_allow_nvidia(cgroup);
	sc_udev_allow_uhid(cgroup);
	sc_udev_allow_dev_net_tun(cgroup);
}

static char *sc_security_to_udev_tag(const char *security_tag)
{
	char *udev_tag = sc_strdup(security_tag);
	for (char *c = strchr(udev_tag, '.'); c != NULL; c = strchr(c, '.')) {
		*c = '_';
	}
	return udev_tag;
}

static void sc_cleanup_udev(struct udev **udev)
{
	if (udev != NULL && *udev != NULL) {
		udev_unref(*udev);
		*udev = NULL;
	}
}

static void sc_cleanup_udev_enumerate(struct udev_enumerate **enumerate)
{
	if (enumerate != NULL && *enumerate != NULL) {
		udev_enumerate_unref(*enumerate);
		*enumerate = NULL;
	}
}

static sc_cgroup_fds sc_udev_open_cgroup_v1(const char *security_tag)
{
	/* Note that -1 is the neutral value for a file descriptor.
	 * This is relevant as a cleanup handler for sc_cgroup_fds,
	 * closes all file descriptors that are not -1. */
	sc_cgroup_fds fds = { -1, -1, -1 };

	/* Open /sys/fs/cgroup */
	const char *cgroup_path = "/sys/fs/cgroup";
	int SC_CLEANUP(sc_cleanup_close) cgroup_fd = -1;
	cgroup_fd = open(cgroup_path,
			 O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (cgroup_fd < 0) {
		die("cannot open %s", cgroup_path);
	}

	/* Open devices relative to /sys/fs/cgroup */
	const char *devices_relpath = "devices";
	int SC_CLEANUP(sc_cleanup_close) devices_fd = -1;
	devices_fd = openat(cgroup_fd, devices_relpath,
			    O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (devices_fd < 0) {
		die("cannot open %s/%s", cgroup_path, devices_relpath);
	}

	/* Open snap.$SNAP_NAME.$APP_NAME relative to /sys/fs/cgroup/devices,
	 * creating the directory if necessary. Note that we always chown the
	 * resulting directory to root:root. */
	const char *security_tag_relpath = security_tag;
	sc_identity old = sc_set_effective_identity(sc_root_group_identity());
	if (mkdirat(devices_fd, security_tag_relpath, 0755) < 0) {
		if (errno != EEXIST) {
			die("cannot create directory %s/%s/%s", cgroup_path,
			    devices_relpath, security_tag_relpath);
		}
	}
	(void)sc_set_effective_identity(old);

	int SC_CLEANUP(sc_cleanup_close) security_tag_fd = -1;
	security_tag_fd = openat(devices_fd, security_tag_relpath,
				 O_RDONLY | O_DIRECTORY | O_CLOEXEC |
				 O_NOFOLLOW);
	if (security_tag_fd < 0) {
		die("cannot open %s/%s/%s", cgroup_path, devices_relpath,
		    security_tag_relpath);
	}

	/* Open devices.allow relative to /sys/fs/cgroup/devices/snap.$SNAP_NAME.$APP_NAME */
	const char *devices_allow_relpath = "devices.allow";
	int SC_CLEANUP(sc_cleanup_close) devices_allow_fd = -1;
	devices_allow_fd = openat(security_tag_fd, devices_allow_relpath,
				  O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
	if (devices_allow_fd < 0) {
		die("cannot open %s/%s/%s/%s", cgroup_path, devices_relpath,
		    security_tag_relpath, devices_allow_relpath);
	}

	/* Open devices.deny relative to /sys/fs/cgroup/devices/snap.$SNAP_NAME.$APP_NAME */
	const char *devices_deny_relpath = "devices.deny";
	int SC_CLEANUP(sc_cleanup_close) devices_deny_fd = -1;
	devices_deny_fd = openat(security_tag_fd, devices_deny_relpath,
				 O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
	if (devices_deny_fd < 0) {
		die("cannot open %s/%s/%s/%s", cgroup_path, devices_relpath,
		    security_tag_relpath, devices_deny_relpath);
	}

	/* Open cgroup.procs relative to /sys/fs/cgroup/devices/snap.$SNAP_NAME.$APP_NAME */
	const char *cgroup_procs_relpath = "cgroup.procs";
	int SC_CLEANUP(sc_cleanup_close) cgroup_procs_fd = -1;
	cgroup_procs_fd = openat(security_tag_fd, cgroup_procs_relpath,
				 O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
	if (cgroup_procs_fd < 0) {
		die("cannot open %s/%s/%s/%s", cgroup_path, devices_relpath,
		    security_tag_relpath, cgroup_procs_relpath);
	}

	/* Everything worked so pack the result and "move" the descriptors over so
	 * that they are not closed by the cleanup functions associated with the
	 * individual variables. */
	fds.devices_allow_fd = devices_allow_fd;
	fds.devices_deny_fd = devices_deny_fd;
	fds.cgroup_procs_fd = cgroup_procs_fd;
	/* Reset the locals so that they are not closed by the cleanup handlers. */
	devices_allow_fd = -1;
	devices_deny_fd = -1;
	cgroup_procs_fd = -1;
	return fds;
}

static void sc_cleanup_cgroup_fds(sc_cgroup_fds * fds)
{
	if (fds != NULL) {
		sc_cleanup_close(&fds->devices_allow_fd);
		sc_cleanup_close(&fds->devices_deny_fd);
		sc_cleanup_close(&fds->cgroup_procs_fd);
	}
}

void sc_setup_device_cgroup(const char *security_tag)
{
	debug("setting up device cgroup");

	/* Derive the udev tag from the snap security tag.
	 *
	 * Because udev does not allow for dots in tag names, those are replaced by
	 * underscores in snapd. We just match that behavior. */
	char *udev_tag SC_CLEANUP(sc_cleanup_string) = NULL;
	udev_tag = sc_security_to_udev_tag(security_tag);

	/* Use udev APIs to talk to udev-the-daemon to determine the list of
	 * "devices" with that tag assigned. The list may be empty, in which case
	 * there's no udev tagging in effect and we must refrain from constructing
	 * the cgroup as it would interfere with the execution of a program. */
	struct udev SC_CLEANUP(sc_cleanup_udev) * udev = NULL;
	udev = udev_new();
	if (udev == NULL) {
		die("cannot connect to udev");
	}
	struct udev_enumerate SC_CLEANUP(sc_cleanup_udev_enumerate) * devices =
	    NULL;
	devices = udev_enumerate_new(udev);
	if (devices == NULL) {
		die("cannot create udev device enumeration");
	}
	if (udev_enumerate_add_match_tag(devices, udev_tag) < 0) {
		die("cannot add tag match to udev device enumeration");
	}
	if (udev_enumerate_scan_devices(devices) < 0) {
		die("cannot enumerate udev devices");
	}
	/* NOTE: udev_list_entry is bound to life-cycle of the used udev_enumerate */
	struct udev_list_entry *assigned;
	assigned = udev_enumerate_get_list_entry(devices);
	if (assigned == NULL) {
		/* NOTE: Nothing is assigned, don't create or use the device cgroup. */
		debug("no devices tagged with %s, skipping device cgroup setup",
		      udev_tag);
		return;
	}

	/* Note that -1 is the neutral value for a file descriptor.
	 * The cleanup function associated with this variable closes
	 * descriptors other than -1. */
	sc_device_cgroup *cgroup SC_CLEANUP(sc_device_cgroup_cleanup) =
	    sc_device_cgroup_new(security_tag);
	/* Setup the device group access control list */
	sc_udev_setup_acls_common(cgroup);
	for (struct udev_list_entry * entry = assigned; entry != NULL;
	     entry = udev_list_entry_get_next(entry)) {
		const char *path = udev_list_entry_get_name(entry);
		if (path == NULL) {
			die("udev_list_entry_get_name failed");
		}
		struct udev_device *device =
		    udev_device_new_from_syspath(udev, path);
		/** This is a non-fatal error as devices can disappear asynchronously
		 * and on slow devices we may indeed observe a device that no longer
		 * exists.
		 *
		 * Similar debug + continue pattern repeats in all the udev calls in
		 * this function. Related to LP: #1881209 */
		if (device == NULL) {
			debug("cannot find device from syspath %s", path);
			continue;
		}

		sc_udev_allow_assigned_device(cgroup, device);
		udev_device_unref(device);
	}
	/* Move ourselves to the device cgroup */
	sc_device_cgroup_attach_pid(cgroup, getpid());
	debug("associated snap application process %i with device cgroup %s",
	      getpid(), security_tag);
}
