#ifndef HIDAPI_LIBUSB_REPORT_DESCRIPTOR_H
#define HIDAPI_LIBUSB_REPORT_DESCRIPTOR_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum report_descr_type {
	REPORT_DESCR_INPUT = 0x80,
	REPORT_DESCR_OUTPUT = 0x90,
	REPORT_DESCR_FEATURE = 0xB0,
};

struct report_global_state {
	uint32_t report_size;
	uint32_t report_count;
	uint8_t report_id;
	int report_size_set;
	int report_count_set;
};

#define REPORT_GLOBAL_STACK_SIZE 16

static uint32_t get_report_item_data(const uint8_t *report_descriptor, size_t item_offset, size_t data_len)
{
	uint32_t value = 0;

	for (size_t i = 0; i < data_len; i++)
		value |= (uint32_t)report_descriptor[item_offset + 1 + i] << (8 * i);

	return value;
}

/* Retrieves the largest report size (in bytes) from the passed-in report
   descriptor. Returns the size on success, 0 when the descriptor contains no
   reports of the requested type, and -1 for a malformed descriptor. */
static ssize_t get_max_report_size(const uint8_t *report_descriptor, size_t descriptor_size, enum report_descr_type report_type)
{
	struct report_global_state state = {0};
	struct report_global_state state_stack[REPORT_GLOBAL_STACK_SIZE];
	size_t report_bits[256] = {0};
	size_t state_stack_size = 0;
	size_t offset = 0;
	int report_found = 0;
	int report_ids_used = 0;

	if (!report_descriptor && descriptor_size > 0)
		return -1;

	while (offset < descriptor_size) {
		const uint8_t key = report_descriptor[offset];
		size_t data_len;
		size_t key_size;

		if (key == 0xfe) {
			/* Long Item: prefix, data size, long-item tag, then data. */
			if (descriptor_size - offset < 3)
				return -1;
			data_len = report_descriptor[offset + 1];
			key_size = 3;
		} else {
			const uint8_t size_code = key & 0x3;
			data_len = size_code == 3 ? 4 : size_code;
			key_size = 1;
		}

		if (data_len > descriptor_size - offset - key_size)
			return -1;

		if (key != 0xfe) {
			const uint8_t key_cmd = key & 0xfc;
			const uint32_t value = get_report_item_data(report_descriptor, offset, data_len);

			switch (key_cmd) {
			case 0x74: /* Report Size */
				state.report_size = value;
				state.report_size_set = 1;
				break;
			case 0x84: /* Report ID */
				if (data_len != 1 || value == 0)
					return -1;
				state.report_id = (uint8_t)value;
				report_ids_used = 1;
				break;
			case 0x94: /* Report Count */
				state.report_count = value;
				state.report_count_set = 1;
				break;
			case 0xa4: /* Push */
				if (data_len != 0 || state_stack_size == REPORT_GLOBAL_STACK_SIZE)
					return -1;
				state_stack[state_stack_size++] = state;
				break;
			case 0xb4: /* Pop */
				if (data_len != 0 || state_stack_size == 0)
					return -1;
				state = state_stack[--state_stack_size];
				break;
			default:
				if (key_cmd == (uint8_t)report_type) {
					size_t item_bits;

					if (!state.report_count_set || !state.report_size_set)
						return -1;
					if (state.report_count != 0 && state.report_size > SIZE_MAX / state.report_count)
						return -1;

					item_bits = (size_t)state.report_count * state.report_size;
					if (report_bits[state.report_id] > SIZE_MAX - item_bits)
						return -1;

					report_bits[state.report_id] += item_bits;
					report_found = 1;
				}
				break;
			}
		}

		offset += key_size + data_len;
	}

	if (state_stack_size != 0)
		return -1;
	if (!report_found)
		return 0;

	if (report_ids_used) {
		size_t max_bits = 0;
		size_t max_bytes;

		/* Report ID 0 is reserved when Report ID items are used. */
		if (report_bits[0] != 0)
			return -1;

		for (size_t report_id = 1; report_id < 256; report_id++) {
			if (report_bits[report_id] > max_bits)
				max_bits = report_bits[report_id];
		}

		if (max_bits > SIZE_MAX - 7)
			return -1;
		max_bytes = (max_bits + 7) / 8;
		if (max_bytes >= (size_t)PTRDIFF_MAX)
			return -1;
		return (ssize_t)(max_bytes + 1);
	}

	if (report_bits[0] > (size_t)PTRDIFF_MAX - 7)
		return -1;
	return (ssize_t)((report_bits[0] + 7) / 8);
}

#endif
