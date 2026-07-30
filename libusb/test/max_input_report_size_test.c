#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hidapi.h"
#include "../hidapi_libusb_report_descriptor.h"

struct max_report_sizes {
	size_t input;
	size_t output;
	size_t feature;
};

static int parse_expected_report_sizes(const char *filename, struct max_report_sizes *sizes)
{
	FILE *file = fopen(filename, "r");
	int found_input = 0;
	int found_output = 0;
	int found_feature = 0;
	int has_report_id = 0;
	char line[256];

	if (!file) {
		fprintf(stderr, "ERROR: Couldn't open file '%s' for reading: %s\n", filename, strerror(errno));
		return -1;
	}

	while (fgets(line, sizeof(line), file)) {
		unsigned int value;

		if (sscanf(line, "pp_data->caps_info[0]->ReportByteLength   = %u", &value) == 1) {
			sizes->input = value;
			found_input = 1;
		} else if (sscanf(line, "pp_data->caps_info[1]->ReportByteLength   = %u", &value) == 1) {
			sizes->output = value;
			found_output = 1;
		} else if (sscanf(line, "pp_data->caps_info[2]->ReportByteLength   = %u", &value) == 1) {
			sizes->feature = value;
			found_feature = 1;
		} else if (sscanf(line, "pp_data->cap[%*u]->ReportID                     = 0x%x", &value) == 1 && value != 0) {
			has_report_id = 1;
		}
	}

	fclose(file);

	if (!found_input || !found_output || !found_feature) {
		fprintf(stderr, "Missing report-size fields in '%s'\n", filename);
		return -1;
	}

	/* Windows includes a report-ID byte in each ReportByteLength even when
	   report IDs are not used. The libusb result only includes an actual ID. */
	if (!has_report_id) {
		if (sizes->input)
			sizes->input--;
		if (sizes->output)
			sizes->output--;
		if (sizes->feature)
			sizes->feature--;
	}

	return 0;
}

static int append_byte(unsigned char *data, size_t data_size, size_t *data_length, unsigned int value, const char *filename)
{
	if (value > 0xff) {
		fprintf(stderr, "Invalid byte value 0x%x in '%s'\n", value, filename);
		return -1;
	}
	if (*data_length >= data_size) {
		fprintf(stderr, "Report descriptor in '%s' exceeds %zu bytes\n", filename, data_size);
		return -1;
	}

	data[(*data_length)++] = (unsigned char)value;
	return 0;
}

static int parse_hid_decode_record(char *line, unsigned char *data, size_t data_size, size_t *data_length, const char *filename)
{
	char *token = strtok(line + 2, " \t\r\n");
	char *end;
	unsigned long expected_length;

	if (!token)
		return -1;

	expected_length = strtoul(token, &end, 10);
	if (*end != '\0' || expected_length > data_size)
		return -1;

	while ((token = strtok(NULL, " \t\r\n")) != NULL) {
		unsigned long value;

		if (strlen(token) != 2 || !isxdigit((unsigned char)token[0]) || !isxdigit((unsigned char)token[1]))
			return -1;

		value = strtoul(token, &end, 16);
		if (*end != '\0' || append_byte(data, data_size, data_length, (unsigned int)value, filename) < 0)
			return -1;
	}

	if (*data_length != expected_length) {
		fprintf(stderr, "HID decode record in '%s' declares %lu bytes but contains %zu\n",
			filename, expected_length, *data_length);
		return -1;
	}

	return 0;
}

static int parse_c_hex_bytes(char *line, unsigned char *data, size_t data_size, size_t *data_length, const char *filename)
{
	char *comment = strstr(line, "//");
	char *cursor = line;
	int found = 0;

	if (comment)
		*comment = '\0';

	while ((cursor = strstr(cursor, "0x")) != NULL) {
		if (cursor[2] != '\0' &&
		    cursor[3] != '\0' &&
		    isxdigit((unsigned char)cursor[2]) &&
		    isxdigit((unsigned char)cursor[3]) &&
		    !isxdigit((unsigned char)cursor[4])) {
			char byte_text[3] = {cursor[2], cursor[3], '\0'};
			unsigned int value = (unsigned int)strtoul(byte_text, NULL, 16);

			if (append_byte(data, data_size, data_length, value, filename) < 0)
				return -1;
			found = 1;
			cursor += 4;
		} else {
			cursor += 2;
		}
	}

	return found;
}

static int parse_trailing_hex_bytes(char *line, unsigned char *data, size_t data_size, size_t *data_length, const char *filename)
{
	char *tokens[128];
	size_t token_count = 0;
	size_t first_hex_token;
	char *token;

	for (token = strtok(line, " \t\r\n"); token && token_count < 128; token = strtok(NULL, " \t\r\n"))
		tokens[token_count++] = token;

	first_hex_token = token_count;
	while (first_hex_token > 0) {
		const char *candidate = tokens[first_hex_token - 1];
		if (strlen(candidate) != 2 ||
		    !isxdigit((unsigned char)candidate[0]) ||
		    !isxdigit((unsigned char)candidate[1]))
			break;
		first_hex_token--;
	}

	for (size_t i = first_hex_token; i < token_count; i++) {
		unsigned int value = (unsigned int)strtoul(tokens[i], NULL, 16);
		if (append_byte(data, data_size, data_length, value, filename) < 0)
			return -1;
	}

	return first_hex_token < token_count;
}

static bool read_report_descriptor(const char *filename, unsigned char *data, size_t data_size, size_t *data_length)
{
	char line[HID_API_MAX_REPORT_DESCRIPTOR_SIZE * 4];
	FILE *file = fopen(filename, "r");
	int found_c_hex = 0;

	if (!file) {
		fprintf(stderr, "ERROR: Couldn't open file '%s' for reading: %s\n", filename, strerror(errno));
		return false;
	}
	*data_length = 0;

	/* hid-decode output includes a canonical raw descriptor on its R: line.
	   Prefer it over the preceding commented disassembly. */
	while (fgets(line, sizeof(line), file)) {
		char *cursor = line;
		while (isspace((unsigned char)*cursor))
			cursor++;
		if (cursor[0] == 'R' && cursor[1] == ':') {
			const int result = parse_hid_decode_record(cursor, data, data_size, data_length, filename);
			fclose(file);
			return result == 0;
		}
	}

	/* Several fixtures contain both a raw tool dump and a normalized
	   usbdescreqparser rendering. Prefer the normalized 0xNN form when it is
	   present so the same descriptor is not parsed twice. */
	rewind(file);
	while (fgets(line, sizeof(line), file)) {
		char line_copy[sizeof(line)];
		char *cursor = line;
		int result;

		while (isspace((unsigned char)*cursor))
			cursor++;
		if (*cursor == '#' || (*cursor == '/' && cursor[1] == '/'))
			continue;

		memcpy(line_copy, line, sizeof(line_copy));
		line_copy[sizeof(line_copy) - 1] = '\0';
		result = parse_c_hex_bytes(line_copy, data, data_size, data_length, filename);
		if (result < 0) {
			fclose(file);
			return false;
		}
		if (result > 0)
			found_c_hex = 1;
	}
	if (found_c_hex) {
		fclose(file);
		return true;
	}

	rewind(file);
	while (fgets(line, sizeof(line), file)) {
		char line_copy[sizeof(line)];
		char *cursor = line;

		while (isspace((unsigned char)*cursor))
			cursor++;
		if (*cursor == '#' || (*cursor == '/' && cursor[1] == '/'))
			continue;

		memcpy(line_copy, line, sizeof(line_copy));
		line_copy[sizeof(line_copy) - 1] = '\0';
		if (parse_trailing_hex_bytes(line_copy, data, data_size, data_length, filename) < 0) {
			fclose(file);
			return false;
		}
	}

	fclose(file);
	if (*data_length == 0) {
		fprintf(stderr, "No report-descriptor bytes found in '%s'\n", filename);
		return false;
	}
	return true;
}

static int test_report_descriptor_parser(void)
{
	static const uint8_t missing_report_size[] = {0x95, 0x01, 0x81, 0x00};
	static const uint8_t truncated_item[] = {0x75};
	static const uint8_t output_only[] = {0x75, 0x08, 0x95, 0x01, 0x91, 0x00};
	static const uint8_t repeated_report_id[] = {
		0x85, 0x01, 0x75, 0x08, 0x95, 0x01, 0x81, 0x00,
		0x85, 0x02, 0x95, 0x01, 0x81, 0x00,
		0x85, 0x01, 0x95, 0x02, 0x81, 0x00,
	};

	if (get_max_report_size(missing_report_size, sizeof(missing_report_size), REPORT_DESCR_INPUT) != -1 ||
	    get_max_report_size(truncated_item, sizeof(truncated_item), REPORT_DESCR_INPUT) != -1) {
		fprintf(stderr, "Malformed report descriptor was not rejected\n");
		return -1;
	}
	if (get_max_report_size(output_only, sizeof(output_only), REPORT_DESCR_INPUT) != 0) {
		fprintf(stderr, "Missing input report was not reported as zero-sized\n");
		return -1;
	}
	if (get_max_report_size(repeated_report_id, sizeof(repeated_report_id), REPORT_DESCR_INPUT) != 4) {
		fprintf(stderr, "Repeated report ID fields were not accumulated correctly\n");
		return -1;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	unsigned char report_descriptor[HID_API_MAX_REPORT_DESCRIPTOR_SIZE];
	size_t report_descriptor_size = 0;
	struct max_report_sizes expected = {0};
	struct max_report_sizes computed;
	ssize_t input_size;
	ssize_t output_size;
	ssize_t feature_size;
	int ret = EXIT_SUCCESS;

	if (argc != 3) {
		fprintf(stderr, "Expected 2 arguments ('<>.pp_data' and '<>_real.rpt_desc'), got: %d\n", argc - 1);
		return EXIT_FAILURE;
	}

	printf("Checking: '%s' / '%s'\n", argv[1], argv[2]);

	if (test_report_descriptor_parser() < 0)
		return EXIT_FAILURE;
	if (!read_report_descriptor(argv[2], report_descriptor, sizeof(report_descriptor), &report_descriptor_size))
		return EXIT_FAILURE;
	if (parse_expected_report_sizes(argv[1], &expected) < 0)
		return EXIT_FAILURE;

	input_size = get_max_report_size(report_descriptor, report_descriptor_size, REPORT_DESCR_INPUT);
	output_size = get_max_report_size(report_descriptor, report_descriptor_size, REPORT_DESCR_OUTPUT);
	feature_size = get_max_report_size(report_descriptor, report_descriptor_size, REPORT_DESCR_FEATURE);
	if (input_size < 0 || output_size < 0 || feature_size < 0) {
		fprintf(stderr, "Failed to parse report descriptor '%s'\n", argv[2]);
		return EXIT_FAILURE;
	}

	computed.input = (size_t)input_size;
	computed.output = (size_t)output_size;
	computed.feature = (size_t)feature_size;

	if (expected.input != computed.input) {
		fprintf(stderr, "Failed to compute input size. Got %zu, expected %zu\n", computed.input, expected.input);
		ret = EXIT_FAILURE;
	}
	if (expected.output != computed.output) {
		fprintf(stderr, "Failed to compute output size. Got %zu, expected %zu\n", computed.output, expected.output);
		ret = EXIT_FAILURE;
	}
	if (expected.feature != computed.feature) {
		fprintf(stderr, "Failed to compute feature size. Got %zu, expected %zu\n", computed.feature, expected.feature);
		ret = EXIT_FAILURE;
	}

	if (ret == EXIT_SUCCESS)
		printf("Computed report sizes: %zu, %zu, %zu\n", computed.input, computed.output, computed.feature);

	return ret;
}
