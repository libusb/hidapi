#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include "../hid.c"

static ssize_t parse_max_input_report_size(const char * filename)
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
				fclose(file);
				return (ssize_t)temp_ushort;
			}
		}
	}

	fprintf(stderr, "Unable to find pp_data->caps_info[0]->ReportByteLength in %s\n", filename);
	fclose(file);

	return -1;
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

	ssize_t expected = parse_max_input_report_size(argv[1]);
	if (expected < 0) {
		fprintf(stderr, "Unable to expected max input report size from %s\n", argv[1]);
		return EXIT_FAILURE;
	}

	ssize_t res = (ssize_t)get_max_input_report_size(report_descriptor, report_descriptor_size);

	if (res != expected) {
		fprintf(stderr, "Failed to properly compute size. Got %zd, expected %zd\n", res, expected);
		return EXIT_FAILURE;
	} else {
		printf("Properly computed size: %zd\n", res);
		return EXIT_SUCCESS;
	}
}
