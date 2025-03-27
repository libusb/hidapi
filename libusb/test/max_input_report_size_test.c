#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include "../hid.c"

struct max_report_sizes {
    size_t input;
    size_t output;
    size_t feature;
};

static int parse_max_input_report_size(const char * filename, struct max_report_sizes * sizes)
{
	FILE* file = fopen(filename, "r");
	if (file == NULL) {
		fprintf(stderr, "ERROR: Couldn't open file '%s' for reading: %s\n", filename, strerror(errno));
		return -1;
	}

	char line[256];
	{
		while (fgets(line, sizeof(line), file) != NULL) {
			unsigned short temp_ushort;
			if (sscanf(line, "pp_data->caps_info[0]->ReportByteLength   = %hu\n", &temp_ushort) == 1) {
				sizes->input = (size_t)temp_ushort;
			}
			if (sscanf(line, "pp_data->caps_info[1]->ReportByteLength   = %hu\n", &temp_ushort) == 1) {
				sizes->output = (size_t)temp_ushort;
			}
			if (sscanf(line, "pp_data->caps_info[2]->ReportByteLength   = %hu\n", &temp_ushort) == 1) {
				sizes->feature = (size_t)temp_ushort;
			}
		}
	}

	fclose(file);

	return 0;
}

static bool read_hex_data_from_text_file(const char *filename, unsigned char *data_out, size_t data_size, size_t *actual_read)
{
	size_t read_index = 0;
	FILE* file = fopen(filename, "r");
	if (file == NULL) {
		fprintf(stderr, "ERROR: Couldn't open file '%s' for reading: %s\n", filename, strerror(errno));
		return false;
	}

	bool result = true;
	unsigned int val;
	char buf[16];
	while (fscanf(file, "%15s", buf) == 1) {
		if (sscanf(buf, "0x%X", &val) != 1) {
			fprintf(stderr, "Invalid HEX text ('%s') file, got %s\n", filename, buf);
			result = false;
			goto end;
		}

		if (read_index >= data_size) {
			fprintf(stderr, "Buffer for file read is too small. Got only %zu bytes to read '%s'\n", data_size, filename);
			result = false;
			goto end;
		}

		if (val > (unsigned char)-1) {
			fprintf(stderr, "Invalid HEX text ('%s') file, got a value of: %u\n", filename, val);
			result = false;
			goto end;
		}

		data_out[read_index] = (unsigned char) val;

		read_index++;
	}

	if (!feof(file)) {
		fprintf(stderr, "Invalid HEX text ('%s') file - failed to read all values\n", filename);
		result = false;
		goto end;
	}

	*actual_read = read_index;

end:
	fclose(file);
	return result;
}


int main(int argc, char* argv[])
{
	if (argc != 3) {
		fprintf(stderr, "Expected 2 arguments for the test ('<>.pp_data' and '<>_expected.rpt_desc'), got: %d\n", argc - 1);
		return EXIT_FAILURE;
	}

	printf("Checking: '%s' / '%s'\n", argv[1], argv[2]);

	unsigned char report_descriptor[HID_API_MAX_REPORT_DESCRIPTOR_SIZE];
	size_t report_descriptor_size = 0;
	if (!read_hex_data_from_text_file(argv[2], report_descriptor, sizeof(report_descriptor), &report_descriptor_size)) {
		return EXIT_FAILURE;
	}

        struct max_report_sizes expected;
	if (parse_max_input_report_size(argv[1], &expected) < 0) {
		fprintf(stderr, "Unable to get expected max report sizes from %s\n", argv[1]);
		return EXIT_FAILURE;
	}

	struct max_report_sizes computed = {
		.input = (size_t)get_max_report_size(report_descriptor, report_descriptor_size, REPORT_DESCR_INPUT),
		.output = (size_t)get_max_report_size(report_descriptor, report_descriptor_size, REPORT_DESCR_OUTPUT),
		.feature = (size_t)get_max_report_size(report_descriptor, report_descriptor_size, REPORT_DESCR_FEATURE)
	};

	int ret = EXIT_SUCCESS;

	if (expected.input != computed.input) {
		fprintf(stderr, "Failed to properly compute input size. Got %zu, expected %zu\n", computed.input, expected.input);
		ret = EXIT_FAILURE;
	}
	if (expected.output != computed.output) {
		fprintf(stderr, "Failed to properly compute output size. Got %zu, expected %zu\n", computed.output, expected.output);
		ret = EXIT_FAILURE;
	}
	if (expected.feature != computed.feature) {
		fprintf(stderr, "Failed to properly compute feature size. Got %zu, expected %zu\n", computed.feature, expected.feature);
		ret = EXIT_FAILURE;
	}

	if (ret == EXIT_SUCCESS) {
		printf("Properly computed sizes: %zu, %zu, %zu\n", computed.input, computed.output, computed.feature);
	}

	return ret;
}
