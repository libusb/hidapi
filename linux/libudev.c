/* SPDX-FileCopyrightText: 2026 Filipe Coelho <falktx@falktx.com> */
/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <dlfcn.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>

#define C(FN)                            \
	static typeof(FN)* FN;               \
	if (FN == NULL) {                    \
		void *realfn = dlsym(NULL, #FN); \
		FN = *(typeof(FN)*)&realfn;      \
	}

#define udev_list_entry_foreach(varname, front) \
	for (varname = front; varname != NULL; varname = udev_list_entry_get_next(varname))

struct udev *udev_new(void)
{
	static void* _lib;
	if (_lib == NULL)
	{
		_lib = dlopen("libudev.so.1", RTLD_NOW | RTLD_GLOBAL);
		if (_lib == NULL)
			return NULL;
	}

	C(udev_new);
	return udev_new();
}

struct udev *udev_unref(struct udev *udev)
{
	C(udev_unref);
	return udev_unref(udev);
}

struct udev_device *udev_device_new_from_devnum(struct udev *udev, char type, dev_t devnum)
{
	C(udev_device_new_from_devnum);
	return udev_device_new_from_devnum(udev, type, devnum);
}

struct udev_device *udev_device_new_from_syspath(struct udev *udev, const char *syspath)
{
	C(udev_device_new_from_syspath);
	return udev_device_new_from_syspath(udev, syspath);
}

struct udev_device *udev_device_unref(struct udev_device *udev_device)
{
	C(udev_device_unref);
	return udev_device_unref(udev_device);
}

struct udev_device *udev_device_get_parent_with_subsystem_devtype(struct udev_device *udev_device,
                                                                  const char *subsystem, const char *devtype)
{
	C(udev_device_get_parent_with_subsystem_devtype);
	return udev_device_get_parent_with_subsystem_devtype(udev_device, subsystem, devtype);
}

const char* udev_device_get_syspath(struct udev_device *udev_device)
{
	C(udev_device_get_syspath);
	return udev_device_get_syspath(udev_device);
}

const char* udev_device_get_devnode(struct udev_device *udev_device)
{
	C(udev_device_get_devnode);
	return udev_device_get_devnode(udev_device);
}

const char* udev_device_get_sysattr_value(struct udev_device *udev_device, const char *sysattr)
{
	C(udev_device_get_sysattr_value);
	return udev_device_get_sysattr_value(udev_device, sysattr);
}

struct udev_enumerate *udev_enumerate_new(struct udev *udev)
{
	C(udev_enumerate_new);
	return udev_enumerate_new(udev);
}

struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *udev_enumerate)
{
	C(udev_enumerate_unref);
	return udev_enumerate_unref(udev_enumerate);
}

int udev_enumerate_add_match_subsystem(struct udev_enumerate *udev_enumerate, const char *subsystem)
{
	C(udev_enumerate_add_match_subsystem);
	return udev_enumerate_add_match_subsystem(udev_enumerate, subsystem);
}

int udev_enumerate_scan_devices(struct udev_enumerate *udev_enumerate)
{
	C(udev_enumerate_scan_devices);
	return udev_enumerate_scan_devices(udev_enumerate);
}

struct udev_list_entry *udev_enumerate_get_list_entry(struct udev_enumerate *udev_enumerate)
{
	C(udev_enumerate_get_list_entry);
	return udev_enumerate_get_list_entry(udev_enumerate);
}

struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *list_entry)
{
	C(udev_list_entry_get_next);
	return udev_list_entry_get_next(list_entry);
}

const char* udev_list_entry_get_name(struct udev_list_entry *list_entry)
{
	C(udev_list_entry_get_name);
	return udev_list_entry_get_name(list_entry);
}

#undef C
