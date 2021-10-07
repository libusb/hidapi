#include "../windows/hid.c"
#include <hidapi.h>

read_preparsed_data_from_file(char* filename, struct hid_device_info* dev, PHIDP_PREPARSED_DATA* pp_data2) {
	FILE* file;
	errno_t err = fopen_s(&file, filename, "r");

	if (err == 0) {
		printf("Opened %s for reading\n", filename);
		char line[256];

		USHORT FirstByteOfLinkCollectionArray;
		USHORT NumberLinkCollectionNodes;

		while (fgets(line, sizeof(line), file) != NULL) {

			if (sscanf(line, "dev->vendor_id           = 0x%04hX\n", &dev->vendor_id)) continue;
			if (sscanf(line, "dev->product_id          = 0x%04hX\n", &dev->product_id)) continue;
			if (sscanf(line, "dev->usage               = 0x%04hX\n", &dev->usage)) continue;
			if (sscanf(line, "dev->usage_page          = 0x%04hX\n", &dev->usage_page)) continue;
			//if (sscanf(line, "dev->manufacturer_string = \"%ls\"\n", &dev->manufacturer_string)) continue;
			//if (sscanf(line, "dev->product_string      = \"%ls\"\n", &dev->product_string)) continue;
			if (sscanf(line, "dev->release_number      = 0x%04hX\n", &dev->release_number)) continue;
			if (sscanf(line, "dev->interface_number    = %d\n", &dev->interface_number)) continue;
				//if (sscanf(line, "dev->path                = \"%s\"\n", &dev->path)) continue;
			if (sscanf(line, "pp_data->FirstByteOfLinkCollectionArray       = 0x%04hX\n", &FirstByteOfLinkCollectionArray)) continue;
			if (sscanf(line, "pp_data->NumberLinkCollectionNodes            = %hu\n", &NumberLinkCollectionNodes)) continue;
		}
		fclose(file);
		
		int sizeOfPreparsedData = offsetof(HIDP_PREPARSED_DATA, caps) + FirstByteOfLinkCollectionArray + (NumberLinkCollectionNodes * sizeof(struct _hid_pp_link_collection_node));
		
		PHIDP_PREPARSED_DATA pp_data;
		pp_data = malloc(sizeOfPreparsedData);

		*pp_data2 = pp_data;

		errno_t err = fopen_s(&file, filename, "r");
		if (err == 0) {
			printf("Opened %s for reading\n", filename);
			char line[256];

			HIDP_REPORT_TYPE rt_idx;
			int caps_idx;
			int token_idx;
			int coll_idx;
			USAGE temp_usage;
			BOOLEAN temp_boolean[3];
			UCHAR temp_uchar[3];
			USHORT temp_ushort;
			ULONG temp_ulong;
			LONG temp_long;

			while (fgets(line, sizeof(line), file) != NULL) {

				if (sscanf(line, "pp_data->MagicKey                             = 0x%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX\n", &pp_data->MagicKey[0], &pp_data->MagicKey[1], &pp_data->MagicKey[2], &pp_data->MagicKey[3], &pp_data->MagicKey[4], &pp_data->MagicKey[5], &pp_data->MagicKey[6], &pp_data->MagicKey[7])) continue;
				if (sscanf(line, "pp_data->Usage                                = 0x%04hX\n", &pp_data->Usage)) continue;
				if (sscanf(line, "pp_data->UsagePage                            = 0x%04hX\n", &pp_data->UsagePage)) continue;
				if (sscanf(line, "pp_data->Reserved                             = 0x%04hX%04hX\n", &pp_data->Reserved[0], &pp_data->Reserved[1])) continue;
				
				if (sscanf(line, "pp_data->caps_info[%d]->FirstCap           = %hu\n", &rt_idx, &temp_ushort) == 2) {
					pp_data->caps_info[rt_idx].FirstCap = temp_ushort;
					continue;
				}
				if (sscanf(line, "pp_data->caps_info[%d]->LastCap            = %hu\n", &rt_idx, &temp_ushort) == 2) {
					pp_data->caps_info[rt_idx].LastCap = temp_ushort;
					continue;
				}
				if (sscanf(line, "pp_data->caps_info[%d]->NumberOfCaps       = %hu\n", &rt_idx, &temp_ushort) == 2) {
					pp_data->caps_info[rt_idx].NumberOfCaps = temp_ushort;
					continue;
				}
				if (sscanf(line, "pp_data->caps_info[%d]->ReportByteLength   = %hu\n", &rt_idx, &temp_ushort) == 2) {
					pp_data->caps_info[rt_idx].ReportByteLength = temp_ushort;
					continue;
				}
				
				if (sscanf(line, "pp_data->FirstByteOfLinkCollectionArray       = 0x%04hX\n", &pp_data->FirstByteOfLinkCollectionArray)) continue;
				if (sscanf(line, "pp_data->NumberLinkCollectionNodes            = %hu\n", &pp_data->NumberLinkCollectionNodes)) continue;
				

				if (sscanf(line, "pp_data->cap[%d]->UsagePage                    = 0x%04hX\n", &caps_idx, &temp_usage) == 2) {
					pp_data->caps[caps_idx].UsagePage = temp_usage;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->ReportID                     = 0x%02hhX\n", &caps_idx, &temp_uchar[0]) == 2) {
					pp_data->caps[caps_idx].ReportID = temp_uchar[0];
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->BitPosition                  = %hhu\n", &caps_idx, &temp_uchar[0]) == 2) {
					pp_data->caps[caps_idx].BitPosition = temp_uchar[0];
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->BitSize                      = %hu\n", &caps_idx, &temp_ushort) == 2) {
					pp_data->caps[caps_idx].BitSize = temp_ushort;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->ReportCount                  = %hu\n", &caps_idx, &temp_ushort) == 2) {
					pp_data->caps[caps_idx].ReportCount = temp_ushort;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->BytePosition                 = 0x%04hX\n", &caps_idx, &temp_ushort) == 2) {
					pp_data->caps[caps_idx].BytePosition = temp_ushort;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->BitCount                     = %hu\n", &caps_idx, &temp_ushort) == 2) {
					pp_data->caps[caps_idx].BitCount = temp_ushort;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->BitField                     = 0x%02X\n", &caps_idx, &temp_ulong) == 2) {
					pp_data->caps[caps_idx].BitField = temp_ulong;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->NextBytePosition             = 0x%04hX\n", &caps_idx, &temp_ushort) == 2) {
					pp_data->caps[caps_idx].NextBytePosition = temp_ushort;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->LinkCollection               = 0x%04hX\n", &caps_idx, &temp_ushort) == 2) {
					pp_data->caps[caps_idx].LinkCollection = temp_ushort;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->LinkUsagePage                = 0x%04hX\n", &caps_idx, &temp_usage) == 2) {
					pp_data->caps[caps_idx].LinkUsagePage = temp_usage;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->LinkUsage                    = 0x%04hX\n", &caps_idx, &temp_usage) == 2) {
					pp_data->caps[caps_idx].LinkUsage = temp_usage;
					continue;
				};

				// 8 Flags in one byte
				if (sscanf(line, "pp_data->cap[%d]->IsMultipleItemsForArray      = %hhu\n", &caps_idx, &temp_boolean[0]) == 2) {
					pp_data->caps[caps_idx].IsMultipleItemsForArray = temp_boolean[0];
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->IsButtonCap                  = %hhu\n", &caps_idx, &temp_boolean[0]) == 2) {
					pp_data->caps[caps_idx].IsButtonCap = temp_boolean[0];
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->IsPadding                    = %hhu\n", &caps_idx, &temp_boolean[0]) == 2) {
					pp_data->caps[caps_idx].IsPadding = temp_boolean[0];
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->IsAbsolute                   = %hhu\n", &caps_idx, &temp_boolean[0]) == 2) {
					pp_data->caps[caps_idx].IsAbsolute = temp_boolean[0];
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->IsRange                      = %hhu\n", &caps_idx, &temp_boolean[0]) == 2) {
					pp_data->caps[caps_idx].IsRange = temp_boolean[0];
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->IsAlias                      = %hhu\n", &caps_idx, &temp_boolean[0]) == 2) {
					pp_data->caps[caps_idx].IsAlias = temp_boolean[0];
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->IsStringRange                = %hhu\n", &caps_idx, &temp_boolean[0]) == 2) {
					pp_data->caps[caps_idx].IsStringRange = temp_boolean[0];
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->IsDesignatorRange            = %hhu\n", &caps_idx, &temp_boolean[0]) == 2) {
					pp_data->caps[caps_idx].IsDesignatorRange = temp_boolean[0];
					continue;
				};

				if (sscanf(line, "pp_data->cap[%d]->Reserved1                    = 0x%hhu%hhu%hhu\n", &caps_idx, &temp_uchar[0], &temp_uchar[1], &temp_uchar[2]) == 4) {
					pp_data->caps[caps_idx].Reserved1[0] = temp_uchar[0];
					pp_data->caps[caps_idx].Reserved1[1] = temp_uchar[1];
					pp_data->caps[caps_idx].Reserved1[2] = temp_uchar[2];
					continue;
				};


				if (sscanf(line, "pp_data->cap[%d]->pp_cap->UnknownTokens[%d].Token    = 0x%02hhX\n", &caps_idx, &token_idx, &temp_uchar[0]) == 3) {
					pp_data->caps[caps_idx].UnknownTokens[token_idx].Token = temp_uchar[0];
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->pp_cap->UnknownTokens[%d].Reserved = 0x%02hhX%02hhX%02hhX\n", &caps_idx, &token_idx, &temp_uchar[0], &temp_uchar[1], &temp_uchar[2]) == 5) {
					pp_data->caps[caps_idx].UnknownTokens[token_idx].Reserved[0] = temp_uchar[0];
					pp_data->caps[caps_idx].UnknownTokens[token_idx].Reserved[1] = temp_uchar[1];
					pp_data->caps[caps_idx].UnknownTokens[token_idx].Reserved[2] = temp_uchar[2];
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->pp_cap->UnknownTokens[%d].BitField = 0x%08X\n", &caps_idx, &token_idx, &temp_ulong) == 3) {
					pp_data->caps[caps_idx].UnknownTokens[token_idx].BitField = temp_ulong;
					continue;
				};

				// Range
				if (sscanf(line, "pp_data->cap[%d]->Range.UsageMin                     = 0x%04hX\n", &caps_idx, &temp_usage) == 2) {
					pp_data->caps[caps_idx].Range.UsageMin = temp_usage;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->Range.UsageMax                     = 0x%04hX\n", &caps_idx, &temp_usage) == 2) {
					pp_data->caps[caps_idx].Range.UsageMax = temp_usage;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->Range.StringMin                    = %hu\n", &caps_idx, &temp_ushort) == 2) {
					pp_data->caps[caps_idx].Range.StringMin = temp_ushort;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->Range.StringMax                    = %hu\n", &caps_idx, &temp_ushort) == 2) {
					pp_data->caps[caps_idx].Range.StringMax = temp_ushort;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->Range.DesignatorMin                = %hu\n", &caps_idx, &temp_ushort) == 2) {
					pp_data->caps[caps_idx].Range.DesignatorMin = temp_ushort;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->Range.DesignatorMax                = %hu\n", &caps_idx, &temp_ushort) == 2) {
					pp_data->caps[caps_idx].Range.DesignatorMax = temp_ushort;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->Range.DataIndexMin                 = %hu\n", &caps_idx, &temp_ushort) == 2) {
					pp_data->caps[caps_idx].Range.DataIndexMin = temp_ushort;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->Range.DataIndexMax                 = %hu\n", &caps_idx, &temp_ushort) == 2) {
					pp_data->caps[caps_idx].Range.DataIndexMax = temp_ushort;
					continue;
				};

				// NotRange
				if (sscanf(line, "pp_data->cap[%d]->NotRange.Usage                        = 0x%04hX\n", &caps_idx, &temp_usage) == 2) {
					pp_data->caps[caps_idx].NotRange.Usage = temp_usage;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->NotRange.Reserved1                    = 0x%04hX\n", &caps_idx, &temp_usage) == 2) {
					pp_data->caps[caps_idx].NotRange.Reserved1 = temp_usage;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->NotRange.StringIndex                  = %hu\n", &caps_idx, &temp_ushort) == 2) {
					pp_data->caps[caps_idx].NotRange.StringIndex = temp_ushort;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->NotRange.Reserved2                    = %hu\n", &caps_idx, &temp_ushort) == 2) {
					pp_data->caps[caps_idx].NotRange.Reserved2 = temp_ushort;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->NotRange.DesignatorIndex              = %hu\n", &caps_idx, &temp_ushort) == 2) {
					pp_data->caps[caps_idx].NotRange.DesignatorIndex = temp_ushort;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->NotRange.Reserved3                    = %hu\n", &caps_idx, &temp_ushort) == 2) {
					pp_data->caps[caps_idx].NotRange.Reserved3 = temp_ushort;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->NotRange.DataIndex                    = %hu\n", &caps_idx, &temp_ushort) == 2) {
					pp_data->caps[caps_idx].NotRange.DataIndex = temp_ushort;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->NotRange.Reserved4                    = %hu\n", &caps_idx, &temp_ushort) == 2) {
					pp_data->caps[caps_idx].NotRange.Reserved4 = temp_ushort;
					continue;
				};

				// Button
				if (sscanf(line, "pp_data->cap[%d]->Button.LogicalMin                   = %d\n", &caps_idx, &temp_long) == 2) {
					pp_data->caps[caps_idx].Button.LogicalMin = temp_long;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->Button.LogicalMax                   = %d\n", &caps_idx, &temp_long) == 2) {
					pp_data->caps[caps_idx].Button.LogicalMax = temp_long;
					continue;
				};

				// NotButton
				if (sscanf(line, "pp_data->cap[%d]->NotButton.HasNull                   = %hhu\n", &caps_idx, &temp_boolean[0]) == 2) {
					pp_data->caps[caps_idx].NotButton.HasNull = temp_boolean[0];
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->NotButton.Reserved4                 = 0x%02hhX%02hhX%02hhX\n", &caps_idx, &temp_uchar[0], &temp_uchar[1], &temp_uchar[2]) == 4) {
					pp_data->caps[caps_idx].NotButton.Reserved4[0] = temp_uchar[0];
					pp_data->caps[caps_idx].NotButton.Reserved4[1] = temp_uchar[1];
					pp_data->caps[caps_idx].NotButton.Reserved4[2] = temp_uchar[2];
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->NotButton.LogicalMin                = %d\n", &caps_idx, &temp_long) == 2) {
					pp_data->caps[caps_idx].NotButton.LogicalMin = temp_long;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->NotButton.LogicalMax                = %d\n", &caps_idx, &temp_long) == 2) {
					pp_data->caps[caps_idx].NotButton.LogicalMax = temp_long;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->NotButton.PhysicalMin               = %d\n", &caps_idx, &temp_long) == 2) {
					pp_data->caps[caps_idx].NotButton.PhysicalMin = temp_long;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->NotButton.PhysicalMax               = %d\n", &caps_idx, &temp_long) == 2) {
					pp_data->caps[caps_idx].NotButton.PhysicalMax = temp_long;
					continue;
				};


				if (sscanf(line, "pp_data->cap[%d]->Units                    = %u\n", &caps_idx, &temp_ulong) == 2) {
					pp_data->caps[caps_idx].Units = temp_ulong;
					continue;
				};
				if (sscanf(line, "pp_data->cap[%d]->UnitsExp                 = %u\n", &caps_idx, &temp_ulong) == 2) {
					pp_data->caps[caps_idx].UnitsExp = temp_ulong;
					continue;
				};

								
			if (sscanf(line, "pp_data->LinkCollectionArray[%d]->LinkUsage          = 0x%04hX\n", &coll_idx, &temp_usage) == 2) {
				pp_data->LinkCollectionArray[coll_idx].LinkUsage = temp_usage;
				continue;
			};
			if (sscanf(line, "pp_data->LinkCollectionArray[%d]->LinkUsagePage      = 0x%04hX\n", &coll_idx, &temp_usage) == 2) {
				pp_data->LinkCollectionArray[coll_idx].LinkUsagePage = temp_usage;
				continue;
			}
			if (sscanf(line, "pp_data->LinkCollectionArray[%d]->Parent             = %hu\n", &coll_idx, &temp_ushort) == 2) {
				pp_data->LinkCollectionArray[coll_idx].Parent = temp_ushort;
				continue;
			}
			if (sscanf(line, "pp_data->LinkCollectionArray[%d]->NumberOfChildren   = %hu\n", &coll_idx, &temp_ushort) == 2) {
				pp_data->LinkCollectionArray[coll_idx].NumberOfChildren = temp_ushort;
				continue;
			}
			if (sscanf(line, "pp_data->LinkCollectionArray[%d]->NextSibling        = %hu\n", &coll_idx, &temp_ushort) == 2) {
				pp_data->LinkCollectionArray[coll_idx].NextSibling = temp_ushort;
				continue;
			}
			if (sscanf(line, "pp_data->LinkCollectionArray[%d]->FirstChild         = %hu\n", &coll_idx, &temp_ushort) == 2) {
				pp_data->LinkCollectionArray[coll_idx].FirstChild = temp_ushort;
				continue;
			}
			if (sscanf(line, "pp_data->LinkCollectionArray[%d]->CollectionType     = %d\n", &coll_idx, &temp_ulong) == 2) {
				pp_data->LinkCollectionArray[coll_idx].CollectionType = temp_ulong;
				continue;
			}
			if (sscanf(line, "pp_data->LinkCollectionArray[%d]->IsAlias            = %d\n", &coll_idx, &temp_ulong) == 2) {
				pp_data->LinkCollectionArray[coll_idx].IsAlias = temp_ulong;
				continue;
			}
			if (sscanf(line, "pp_data->LinkCollectionArray[%d]->Reserved           = %d\n", &coll_idx, &temp_ulong) == 2) {
				pp_data->LinkCollectionArray[coll_idx].Reserved = temp_ulong;
				continue;
			}


			}
	    fclose(file);
		printf("Read Preparsed Data from %s\n", filename);
		}
	}


}


main(int argc, char* argv[])
{

	int res;
	unsigned char buf[256];
#define MAX_STR 255
	wchar_t wstr[MAX_STR];
	hid_device* handle;
	int i;


	struct hid_device_info* devs, * cur_dev;

	printf("hidapi test/example tool. Compiled with hidapi version %s, runtime version %s.\n", HID_API_VERSION_STR, hid_version_str());
	if (hid_version()->major == HID_API_VERSION_MAJOR && hid_version()->minor == HID_API_VERSION_MINOR && hid_version()->patch == HID_API_VERSION_PATCH) {
		printf("Compile-time version matches runtime version of hidapi.\n\n");
	}
	else {
		printf("Compile-time version is different than runtime version of hidapi.\n]n");
	}

	if (hid_init())
		return -1;

	devs = hid_enumerate(0x0, 0x0);
	cur_dev = devs;
	while (cur_dev) {
		printf("Device Found\n  type: %04hx %04hx\n  path: %s\n  serial_number: %ls", cur_dev->vendor_id, cur_dev->product_id, cur_dev->path, cur_dev->serial_number);
		printf("\n");
		printf("  Manufacturer: %ls\n", cur_dev->manufacturer_string);
		printf("  Product:      %ls\n", cur_dev->product_string);
		printf("  Release:      %hx\n", cur_dev->release_number);
		printf("  Interface:    %d\n", cur_dev->interface_number);
		printf("  Usage (page): 0x%hx (0x%hx)\n", cur_dev->usage, cur_dev->usage_page);

		printf("\n");
		cur_dev = cur_dev->next;
	}
	//hid_get_report_descriptor(hid_device * dev, unsigned char* buf, size_t buf_size)
	printf("%s\n", argv[1]);

	const char dummy[256];
	PHIDP_PREPARSED_DATA pp_data = NULL;
	struct hid_device_info dev_info = { .interface_number = 0,
	.manufacturer_string = dummy,
	.path = dummy};
	read_preparsed_data_from_file(argv[1], &dev_info, &pp_data);
	printf("Device Found\n  type: %04hx %04hx\n  path: %s\n  serial_number: %ls", dev_info.vendor_id, dev_info.product_id, dev_info.path, dev_info.serial_number);
	printf("\n");

	printf("  Manufacturer: %ls\n", dev_info.manufacturer_string);

	unsigned char report_descriptor[4096];

	//hid_get_report_descriptor(handle, report_descriptor, 4096);

	HidD_FreePreparsedData(pp_data);

	printf("\n");

	return 1;
}