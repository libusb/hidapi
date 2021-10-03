#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <stdlib.h>

#include <../windows/hid.c>


void dump_hid_pp_cap(FILE* file, phid_pp_cap pp_cap, unsigned int cap_idx) {
	fprintf(file, "pp_data->cap[%d]->UsagePage                    = 0x%hX\n", cap_idx, pp_cap->UsagePage);
	fprintf(file, "pp_data->cap[%d]->ReportID                     = 0x%hX\n", cap_idx, pp_cap->ReportID);
	fprintf(file, "pp_data->cap[%d]->BitPosition                  = %d\n", cap_idx, pp_cap->BitPosition);
	fprintf(file, "pp_data->cap[%d]->BitSize                      = %d\n", cap_idx, pp_cap->BitSize);
	fprintf(file, "pp_data->cap[%d]->ReportCount                  = %d\n", cap_idx, pp_cap->ReportCount);
	fprintf(file, "pp_data->cap[%d]->BytePosition                 = 0x%hX\n", cap_idx, pp_cap->BytePosition);
	fprintf(file, "pp_data->cap[%d]->BitCount                     = %d\n", cap_idx, pp_cap->BitCount);
	fprintf(file, "pp_data->cap[%d]->BitField                     = 0x%hX\n", cap_idx, pp_cap->BitField);
	fprintf(file, "pp_data->cap[%d]->NextBytePosition             = 0x%hX\n", cap_idx, pp_cap->NextBytePosition);
	fprintf(file, "pp_data->cap[%d]->LinkCollection               = 0x%hX\n", cap_idx, pp_cap->LinkCollection);
	fprintf(file, "pp_data->cap[%d]->LinkUsagePage                = 0x%hX\n", cap_idx, pp_cap->LinkUsagePage);
	fprintf(file, "pp_data->cap[%d]->LinkUsage                    = 0x%hX\n", cap_idx, pp_cap->LinkUsage);

	// 8 Flags in one byte
	fprintf(file, "pp_data->cap[%d]->IsMultipleItemsForArray      = %d\n", cap_idx, pp_cap->IsMultipleItemsForArray);
	fprintf(file, "pp_data->cap[%d]->IsButtonCap                  = %d\n", cap_idx, pp_cap->IsButtonCap);
	fprintf(file, "pp_data->cap[%d]->IsPadding                    = %d\n", cap_idx, pp_cap->IsPadding);
	fprintf(file, "pp_data->cap[%d]->IsAbsolute                   = %d\n", cap_idx, pp_cap->IsAbsolute);
	fprintf(file, "pp_data->cap[%d]->IsRange                      = %d\n", cap_idx, pp_cap->IsRange);
	fprintf(file, "pp_data->cap[%d]->IsAlias                      = %d\n", cap_idx, pp_cap->IsAlias);
	fprintf(file, "pp_data->cap[%d]->IsStringRange                = %d\n", cap_idx, pp_cap->IsStringRange);
	fprintf(file, "pp_data->cap[%d]->IsDesignatorRange            = %d\n", cap_idx, pp_cap->IsDesignatorRange);

	fprintf(file, "pp_data->cap[%d]->Reserved1                    = 0x%hX%hX%hX\n", cap_idx, pp_cap->Reserved1[0], pp_cap->Reserved1[1], pp_cap->Reserved1[2]);

	for (int token_idx = 0; token_idx < 4; token_idx++) {
		fprintf(file, "pp_data->cap[%d]->pp_cap->UnknownTokens[%d].BitField = 0x%hX\n", cap_idx, token_idx, pp_cap->UnknownTokens[token_idx].BitField);
		fprintf(file, "pp_data->cap[%d]->pp_cap->UnknownTokens[%d].Reserved = 0x%hX%hX%hX\n", cap_idx, token_idx, pp_cap->UnknownTokens[token_idx].Reserved[0], pp_cap->UnknownTokens[token_idx].Reserved[1], pp_cap->UnknownTokens[token_idx].Reserved[2]);
		fprintf(file, "pp_data->cap[%d]->pp_cap->UnknownTokens[%d].Token    = 0x%hX\n", cap_idx, token_idx, pp_cap->UnknownTokens[token_idx].Token);
	}

	if (pp_cap->IsRange) {
		fprintf(file, "pp_data->cap[%d]->Range.UsageMin                     = 0x%hX\n", cap_idx, pp_cap->Range.UsageMin);
		fprintf(file, "pp_data->cap[%d]->Range.UsageMax                     = 0x%hX\n", cap_idx, pp_cap->Range.UsageMax);
		fprintf(file, "pp_data->cap[%d]->Range.StringMin                    = 0x%hX\n", cap_idx, pp_cap->Range.StringMin);
		fprintf(file, "pp_data->cap[%d]->Range.StringMax                    = 0x%hX\n", cap_idx, pp_cap->Range.StringMax);
		fprintf(file, "pp_data->cap[%d]->Range.DesignatorMin                = 0x%hX\n", cap_idx, pp_cap->Range.DesignatorMin);
		fprintf(file, "pp_data->cap[%d]->Range.DesignatorMax                = 0x%hX\n", cap_idx, pp_cap->Range.DesignatorMax);
		fprintf(file, "pp_data->cap[%d]->Range.DataIndexMin                 = 0x%hX\n", cap_idx, pp_cap->Range.DataIndexMin);
		fprintf(file, "pp_data->cap[%d]->Range.DataIndexMax                 = 0x%hX\n", cap_idx, pp_cap->Range.DataIndexMax);
	}
	else {
		fprintf(file, "pp_data->cap[%d]->NotRange.Usage                        = 0x%hX\n", cap_idx, pp_cap->NotRange.Usage);
		fprintf(file, "pp_data->cap[%d]->NotRange.Reserved1                    = 0x%hX\n", cap_idx, pp_cap->NotRange.Reserved1);
		fprintf(file, "pp_data->cap[%d]->NotRange.StringIndex                  = 0x%hX\n", cap_idx, pp_cap->NotRange.StringIndex);
		fprintf(file, "pp_data->cap[%d]->NotRange.Reserved2                    = 0x%hX\n", cap_idx, pp_cap->NotRange.Reserved2);
		fprintf(file, "pp_data->cap[%d]->NotRange.DesignatorIndex              = 0x%hX\n", cap_idx, pp_cap->NotRange.DesignatorIndex);
		fprintf(file, "pp_data->cap[%d]->NotRange.Reserved3                    = 0x%hX\n", cap_idx, pp_cap->NotRange.Reserved3);
		fprintf(file, "pp_data->cap[%d]->NotRange.DataIndex                    = 0x%hX\n", cap_idx, pp_cap->NotRange.DataIndex);
		fprintf(file, "pp_data->cap[%d]->NotRange.Reserved4                    = 0x%hX\n", cap_idx, pp_cap->NotRange.Reserved4);
	}

	if (pp_cap->IsButtonCap) {
		fprintf(file, "pp_data->cap[%d]->Button.LogicalMin                   = %d\n", cap_idx, pp_cap->Button.LogicalMin);
		fprintf(file, "pp_data->cap[%d]->Button.LogicalMax                   = %d\n", cap_idx, pp_cap->Button.LogicalMax);
	}
	else
	{
		fprintf(file, "pp_data->cap[%d]->NotButton.HasNull                   = %d\n", cap_idx, pp_cap->NotButton.HasNull);
		fprintf(file, "pp_data->cap[%d]->NotButton.Reserved4                 = 0x%hX%hX%hX\n", cap_idx, pp_cap->NotButton.Reserved4[0], pp_cap->NotButton.Reserved4[1], pp_cap->NotButton.Reserved4[2]);
		fprintf(file, "pp_data->cap[%d]->NotButton.LogicalMin                = %d\n", cap_idx, pp_cap->NotButton.LogicalMin);
		fprintf(file, "pp_data->cap[%d]->NotButton.LogicalMax                = %d\n", cap_idx, pp_cap->NotButton.LogicalMax);
		fprintf(file, "pp_data->cap[%d]->NotButton.PhysicalMin               = %d\n", cap_idx, pp_cap->NotButton.PhysicalMin);
		fprintf(file, "pp_data->cap[%d]->NotButton.PhysicalMax               = %d\n", cap_idx, pp_cap->NotButton.PhysicalMax);
	};
	fprintf(file, "pp_data->cap[%d]->Units                    = %d\n", cap_idx, pp_cap->Units);
	fprintf(file, "pp_data->cap[%d]->UnitsExp                 = %d\n", cap_idx, pp_cap->UnitsExp);
}

void dump_hidp_link_collection_node(FILE* file, PHIDP_LINK_COLLECTION_NODE pcoll, unsigned int coll_idx) {
	fprintf(file, "pp_data->LinkCollectionArray[%d]->LinkUsage          = 0x%hX\n", coll_idx, pcoll->LinkUsage);
	fprintf(file, "pp_data->LinkCollectionArray[%d]->LinkUsagePage      = 0x%hX\n", coll_idx, pcoll->LinkUsagePage);
	fprintf(file, "pp_data->LinkCollectionArray[%d]->Parent             = %d\n", coll_idx, pcoll->Parent);
	fprintf(file, "pp_data->LinkCollectionArray[%d]->NumberOfChildren   = %d\n", coll_idx, pcoll->NumberOfChildren);
	fprintf(file, "pp_data->LinkCollectionArray[%d]->NextSibling        = %d\n", coll_idx, pcoll->NextSibling);
	fprintf(file, "pp_data->LinkCollectionArray[%d]->FirstChild         = %d\n", coll_idx, pcoll->FirstChild);
	fprintf(file, "pp_data->LinkCollectionArray[%d]->CollectionType     = 0x%hX\n", coll_idx, pcoll->CollectionType);
	fprintf(file, "pp_data->LinkCollectionArray[%d]->IsAlias            = %d\n", coll_idx, pcoll->IsAlias);
	fprintf(file, "pp_data->LinkCollectionArray[%d]->Reserved           = 0x%hX\n", coll_idx, pcoll->Reserved);
	fprintf(file, "pp_data->LinkCollectionArray[%d]->UserContext        = 0x%hX\n", coll_idx, pcoll->UserContext);
}

int dump_pp_data(FILE* file, hid_device* dev)
{
	BOOL res;
	PHIDP_PREPARSED_DATA pp_data = NULL;

	res = HidD_GetPreparsedData(dev->device_handle, &pp_data);
	if (!res) {
		register_error(dev, "HidD_GetPreparsedData");
		return -1;
	}
	else {
		fprintf(file, "pp_data->MagicKey                             = 0x%hX%hX%hX%hX%hX%hX%hX%hX\n", pp_data->MagicKey[0], pp_data->MagicKey[1], pp_data->MagicKey[2], pp_data->MagicKey[3], pp_data->MagicKey[4], pp_data->MagicKey[5], pp_data->MagicKey[6], pp_data->MagicKey[7]);
		fprintf(file, "pp_data->Usage                                = 0x%hX\n", pp_data->Usage);
		fprintf(file, "pp_data->UsagePage                            = 0x%hX\n", pp_data->UsagePage);
		fprintf(file, "pp_data->Reserved                             = 0x%hX%hX\n", pp_data->Reserved[0], pp_data->Reserved[1]);
		fprintf(file, "# Input caps_info struct:\n");
		fprintf(file, "pp_data->caps_info[0]->FirstCap           = 0x%hX\n", pp_data->caps_info[0].FirstCap);
		fprintf(file, "pp_data->caps_info[0]->LastCap            = 0x%hX\n", pp_data->caps_info[0].LastCap);
		fprintf(file, "pp_data->caps_info[0]->NumberOfCaps       = 0x%hX\n", pp_data->caps_info[0].NumberOfCaps);
		fprintf(file, "pp_data->caps_info[0]->ReportByteLength   = 0x%hX\n", pp_data->caps_info[0].ReportByteLength);
		fprintf(file, "# Output caps_info struct:\n");
		fprintf(file, "pp_data->caps_info[1]->FirstCap          = 0x%hX\n", pp_data->caps_info[1].FirstCap);
		fprintf(file, "pp_data->caps_info[1]->LastCap           = 0x%hX\n", pp_data->caps_info[1].LastCap);
		fprintf(file, "pp_data->caps_info[1]->NumberOfCaps      = 0x%hX\n", pp_data->caps_info[1].NumberOfCaps);
		fprintf(file, "pp_data->caps_info[1]->ReportByteLength  = 0x%hX\n", pp_data->caps_info[1].ReportByteLength);
		fprintf(file, "# Feature caps_info struct:\n");
		fprintf(file, "pp_data->caps_info[2]->FirstCap         = 0x%hX\n", pp_data->caps_info[2].FirstCap);
		fprintf(file, "pp_data->caps_info[2]->LastCap          = 0x%hX\n", pp_data->caps_info[2].LastCap);
		fprintf(file, "pp_data->caps_info[2]->NumberOfCaps     = 0x%hX\n", pp_data->caps_info[2].NumberOfCaps);
		fprintf(file, "pp_data->caps_info[2]->ReportByteLength = 0x%hX\n", pp_data->caps_info[2].ReportByteLength);
		fprintf(file, "# LinkCollectionArray Offset & Size:\n");
		fprintf(file, "pp_data->FirstByteOfLinkCollectionArray       = 0x%hX\n", pp_data->FirstByteOfLinkCollectionArray);
		fprintf(file, "pp_data->NumberLinkCollectionNodes            = 0x%hX\n", pp_data->NumberLinkCollectionNodes);


		phid_pp_cap pcap = (phid_pp_cap)(((unsigned char*)pp_data) + offsetof(HIDP_PREPARSED_DATA, caps));
		fprintf(file, "# Input hid_pp_cap struct:\n");
		for (int caps_idx = 0; caps_idx < pp_data->caps_info[0].NumberOfCaps; caps_idx++) {
			dump_hid_pp_cap(file, pcap + pp_data->caps_info[0].FirstCap + caps_idx, caps_idx);
			fprintf(file, "\n");
		}
		fprintf(file, "# Output hid_pp_cap struct:\n");
		for (int caps_idx = 0; caps_idx < pp_data->caps_info[1].NumberOfCaps; caps_idx++) {
			dump_hid_pp_cap(file, pcap + pp_data->caps_info[1].FirstCap + caps_idx, caps_idx);
			fprintf(file, "\n");
		}
		fprintf(file, "# Feature hid_pp_cap struct:\n");
		for (int caps_idx = 0; caps_idx < pp_data->caps_info[2].NumberOfCaps; caps_idx++) {
			dump_hid_pp_cap(file, pcap + pp_data->caps_info[2].FirstCap + caps_idx, caps_idx);
			fprintf(file, "\n");
		}

		PHIDP_LINK_COLLECTION_NODE pcoll = (PHIDP_LINK_COLLECTION_NODE)(((unsigned char*)pcap) + pp_data->FirstByteOfLinkCollectionArray);
		fprintf(file, "# Link Collections:\n");
		for (int coll_idx = 0; coll_idx < pp_data->NumberLinkCollectionNodes; coll_idx++) {
			dump_hidp_link_collection_node(file, pcoll + coll_idx, coll_idx);
		}

		HidD_FreePreparsedData(pp_data);
		return 0;
	}
}

int main(int argc, char* argv[])
{
	(void)argc;
	(void)argv;

	int res;
	#define MAX_STR 255

	struct hid_device_info *devs, *cur_dev;

	printf("pp_data_dump tool. Compiled with hidapi version %s, runtime version %s.\n", HID_API_VERSION_STR, hid_version_str());
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
		printf("  Release:      %hX\n", cur_dev->release_number);
		printf("  Interface:    %d\n",  cur_dev->interface_number);
		printf("  Usage (page): 0x%hX (0x%hX)\n", cur_dev->usage, cur_dev->usage_page);

		hid_device *device = hid_open_path(cur_dev->path);
		if (device) {
			char filename[MAX_STR];
			FILE* file;

			sprintf_s(filename, MAX_STR, "%04hx_%04hx_0x%hX_0x%hX.pp_data", cur_dev->vendor_id, cur_dev->product_id, cur_dev->usage, cur_dev->usage_page);
			errno_t err = fopen_s(&file, filename, "w");
			if (err == 0) {
				fprintf(file, "# HIDAPI device info struct:\n");
				fprintf(file, "dev->vendor_id           = 0x%hX\n", cur_dev->vendor_id);
				fprintf(file, "dev->product_id          = 0x%hX\n", cur_dev->product_id);
				fprintf(file, "dev->manufacturer_string = \"%ls\"\n", cur_dev->manufacturer_string);
				fprintf(file, "dev->product_string      = \"%ls\"\n", cur_dev->product_string);
				fprintf(file, "dev->release_number      = 0x%hX\n", cur_dev->release_number);
				fprintf(file, "dev->interface_number    = 0x%hX\n", cur_dev->interface_number);
				fprintf(file, "dev->usage               = 0x%hX\n", cur_dev->usage);
				fprintf(file, "dev->usage_page          = 0x%hX\n", cur_dev->usage_page);
				fprintf(file, "dev->path                = \"%s\"\n", cur_dev->path);
				fprintf(file, "\n# Preparsed Data struct:\n");
				res = dump_pp_data(file, device);


				fclose(file);
				printf("Dumped Preparsed Data to %s\n", filename);
			}

			hid_close(device);
		}
		else {
			printf("  Device: not available.\n");
		}

		printf("\n");
		cur_dev = cur_dev->next;
	}
	hid_free_enumeration(devs);


	/* Free static HIDAPI objects. */
	hid_exit();

	//system("pause");

	return 0;
}
