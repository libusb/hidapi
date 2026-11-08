/*
 * HIDAPI Coverity Scan model. See LICENSE.txt for licensing terms.
 *
 * Analysis input only: upload through Analysis Settings -> Modeling File.
 * Never compile or link this file into HIDAPI.
 *
 * Model the public handle factories, whose implementation varies by backend.
 * Both return an owned handle or NULL. Keep hid_close() and all I/O, error,
 * enumeration and synchronization functions derived from the captured source:
 * their effects include backend-specific resources and borrowed storage.
 *
 * Uninitialized choices intentionally express nondeterminism in Coverity's
 * modeling language. They are not executable C.
 *
 * Contracts: hidapi/hidapi.h (hid_open and hid_open_path).
 * Primitives: https://scan.coverity.com/models
 */

/* Scan compiles this file without system headers. The serial is never read,
 * so its platform-specific character width does not affect this C model. */
typedef int wchar_t;

typedef struct hid_device_ hid_device;

void *__coverity_alloc_nosize__(void);
void __coverity_writeall__(void *ptr);

hid_device *hid_open(unsigned short vendor_id, unsigned short product_id,
                     const wchar_t *serial_number)
{
	int succeeds;
	hid_device *dev;
	(void)vendor_id;
	(void)product_id;
	(void)serial_number;

	if (!succeeds)
		return 0;
	dev = (hid_device *)__coverity_alloc_nosize__();
	__coverity_writeall__(dev);
	return dev;
}

hid_device *hid_open_path(const char *path)
{
	int succeeds;
	hid_device *dev;
	(void)path;

	if (!succeeds)
		return 0;
	dev = (hid_device *)__coverity_alloc_nosize__();
	__coverity_writeall__(dev);
	return dev;
}
