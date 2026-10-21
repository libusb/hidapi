/* Hardware-free regression: exercise the actual libusb callback and queue. */
#include <assert.h>
#include <libusb.h>
static int LIBUSB_CALL fake_submit(struct libusb_transfer *transfer)
{
	(void)transfer;
	return 0;
}
/* Avoid USB discovery entirely, including during library initialization. */
static int LIBUSB_CALL fake_init(libusb_context **context)
{
	*context = (libusb_context *)&fake_init;
	return 0;
}
static void LIBUSB_CALL fake_exit(libusb_context *context) { (void)context; }
#define libusb_init fake_init
#define libusb_exit fake_exit
#define libusb_submit_transfer fake_submit
#include "../../libusb/hid.c"

int main(void)
{
	hid_device *dev;
	unsigned long long received = 99, dropped = 99;
	unsigned int peak = 99;
	unsigned char payload[2], output[2];
	struct libusb_transfer transfer = {0};
	assert(hid_init() == 0);
	dev = new_hid_device();
	assert(dev);
	assert(hid_get_queue_stats(dev, &received, &dropped, &peak) == 0);
	assert(received == 0 && dropped == 0 && peak == 0);
	assert(hid_get_queue_stats(NULL, &received, &dropped, &peak) == -1);
	assert(hid_get_queue_stats(dev, NULL, &dropped, &peak) == -1);
	assert(hid_get_queue_stats(dev, &received, NULL, &peak) == -1);
	assert(hid_get_queue_stats(dev, &received, &dropped, NULL) == -1);
	transfer.user_data = dev;
	transfer.status = LIBUSB_TRANSFER_COMPLETED;
	transfer.buffer = payload;
	transfer.actual_length = sizeof(payload);
	for (unsigned int i = 0; i < 1025; ++i) {
		payload[0] = i & 255;
		payload[1] = i >> 8;
		read_callback(&transfer);
	}
	assert(hid_get_queue_stats(dev, &received, &dropped, &peak) == 0);
	assert(received == 1025 && dropped == 1 && peak == 1025);
	for (unsigned int i = 1; i < 1025; ++i) {
		assert(return_data(dev, output, sizeof(output)) == 2);
		assert(output[0] == (i & 255) && output[1] == (i >> 8));
	}
	assert(dev->input_reports == NULL);
	for (unsigned int i = 1025; i < 22658; ++i) {
		read_callback(&transfer);
		assert(return_data(dev, output, sizeof(output)) == 2);
	}
	assert(hid_get_queue_stats(dev, &received, &dropped, &peak) == 0);
	assert(received == 22658 && dropped == 1 && peak == 1025);
	free_hid_device(dev);
	dev = new_hid_device();
	assert(dev);
	transfer.user_data = dev;
	for (unsigned int i = 0; i < 22658; ++i) {
		read_callback(&transfer);
		assert(return_data(dev, output, sizeof(output)) == 2);
	}
	assert(hid_get_queue_stats(dev, &received, &dropped, &peak) == 0);
	assert(received == 22658 && dropped == 0 && peak == 1);
	free_hid_device(dev);
	assert(hid_exit() == 0);
	return 0;
}
