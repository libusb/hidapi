#include <hidapi.h>

#include <memory>
#include <string>

#include <winrt/base.h>

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.HumanInterfaceDevice.h>
#include <winrt/Windows.Foundation.Collections.h>

namespace WinHid = winrt::Windows::Devices::HumanInterfaceDevice;

// has to be defined in global namespace
struct hid_device_
{
    WinHid::HidDevice handle{nullptr};

    hid_device_(const winrt::hstring &id)
    {
        handle = WinHid::HidDevice::FromIdAsync(id,
                                                winrt::Windows::Storage::FileAccessMode::ReadWrite)
                     .get();
        if (!handle) {
            handle = WinHid::HidDevice::FromIdAsync(id,
                                                    winrt::Windows::Storage::FileAccessMode::Read)
                         .get();
        }

        if (!handle) {
            throw std::runtime_error("Failed to open device: " + winrt::to_string(id));
        }
    }

    bool blocking;
    std::wstring last_error_str;
    struct hid_device_info *device_info = nullptr;
};

namespace {

static const struct hid_api_version api_version = {
    HID_API_VERSION_MAJOR,
    HID_API_VERSION_MINOR,
    HID_API_VERSION_PATCH //
};

static std::wstring &GlobalErrorStr()
{
    static std::wstring global_error;
    return global_error;
}

struct HidDeviceInfoExt : public hid_device_info
{
    HidDeviceInfoExt()
        : hid_device_info{} // zero-initialized
    {
        interface_number = -1;
    }

    std::string owned_path;
    winrt::hstring owned_product_string;
};

auto WinRTHidEnumerate(unsigned short vendor_id, unsigned short product_id)
{
    namespace WinEnumeration = winrt::Windows::Devices::Enumeration;

    std::wstring enumeration_selector
        = L"System.Devices.InterfaceClassGuid:=\"{4D1E55B2-F16F-11CF-88CB-"
          L"001111000030}\" AND "
          L"System.Devices.InterfaceEnabled:=System.StructuredQueryType."
          L"Boolean#True";

    // enumeration_selector = WinHid::HidDevice::GetDeviceSelector(0, 0, vendor_id, product_id);
    // L" AND System.DeviceInterface.Hid.UsagePage:=0"
    // L" AND System.DeviceInterface.Hid.UsageId:=0"

    if (vendor_id != 0) {
        enumeration_selector += L" AND System.DeviceInterface.Hid.VendorId:=";
        enumeration_selector += std::to_wstring(vendor_id);
    }
    if (vendor_id != 0) {
        enumeration_selector += L" AND System.DeviceInterface.Hid.ProductId:=";
        enumeration_selector += std::to_wstring(product_id);
    }

    return WinEnumeration::DeviceInformation::FindAllAsync(enumeration_selector).get();
}

hid_device_info *do_hid_enumerate( //
    unsigned short vendor_id,
    unsigned short product_id)
{
    using namespace winrt;

    auto devices = WinRTHidEnumerate(vendor_id, product_id);

    std::unique_ptr<hid_device_info, decltype(&hid_free_enumeration)> result(nullptr,
                                                                             ::hid_free_enumeration);

    for (const auto &device : devices) {
        auto info = std::make_unique<HidDeviceInfoExt>();
        info->owned_path = to_string(device.Id());
        info->path = info->owned_path.data();
        info->owned_product_string = device.Name();
        info->product_string = const_cast<wchar_t *>(info->owned_product_string.c_str());

        auto dev_h = WinHid::HidDevice::FromIdAsync(device.Id(),
                                                    winrt::Windows::Storage::FileAccessMode::Read)
                         .get();

        if (dev_h) {
            info->vendor_id = dev_h.VendorId();
            info->product_id = dev_h.ProductId();
            info->release_number = dev_h.Version();
            info->usage_page = dev_h.UsagePage();
            info->usage = dev_h.UsageId();
        }

        info->next = result.get();
        result.release();
        result.reset(info.release());
    }

    return result.release();
}

hid_device *do_hid_open( //
    unsigned short vendor_id,
    unsigned short product_id)
{
    auto devices = WinRTHidEnumerate(vendor_id, product_id);

    if (devices.Size() == 0) {
        if (vendor_id != 0 || product_id != 0) {
            GlobalErrorStr() = L"No devices found with specified VID/PID";
        } else {
            GlobalErrorStr() = L"No devices found in the system";
        }
        return nullptr;
    }

    return new hid_device_(devices.GetAt(0).Id());
}

} // namespace

HID_API_EXPORT const struct hid_api_version *HID_API_CALL hid_version()
{
    return &api_version;
}

HID_API_EXPORT const char *HID_API_CALL hid_version_str()
{
    return HID_API_VERSION_STR;
}

int HID_API_EXPORT hid_init(void)
{
    using namespace winrt;

    try {
        init_apartment(apartment_type::multi_threaded);
        GlobalErrorStr().clear();
        return 0;
    } catch (const hresult_error &e) {
        if (e.code() == hresult(int32_t(0x80010106))) { // RPC_E_CHANGED_MODE
            GlobalErrorStr() = L"Multithreaded Windows Runtime Apartment required, "
                               "but single thread mode already initialized";
        } else {
            GlobalErrorStr() = L"Multithreaded Windows Runtime Apartment initializing error: "
                               + e.message();
        }
    } catch (const std::exception &e) {
        GlobalErrorStr() = L"Multithreaded Windows Runtime Apartment initializing error: "
                           + to_hstring(e.what());
    } catch (...) {
        GlobalErrorStr() = L"Multithreaded Windows Runtime Apartment initializing error: UNKNOWN";
    }
    return -1;
}

int HID_API_EXPORT hid_exit(void)
{
    GlobalErrorStr().clear();
    using namespace winrt;

    // More details here: https://kennykerr.ca/2018/03/24/cppwinrt-hosting-the-windows-runtime/
    clear_factory_cache();

    uninit_apartment();
    return 0;
}

struct hid_device_info HID_API_EXPORT *HID_API_CALL hid_enumerate( //
    unsigned short vendor_id,
    unsigned short product_id)
{
    using namespace winrt;

    try {
        auto result = do_hid_enumerate(vendor_id, product_id);
        if (result)
            GlobalErrorStr().clear();
        return result;
    } catch (const hresult_error &e) {
        GlobalErrorStr() = L"hid_enumerate error: ";
        GlobalErrorStr() += e.message();
    } catch (const std::exception &e) {
        GlobalErrorStr() = L"hid_enumerate error: ";
        GlobalErrorStr() += to_hstring(e.what());
    } catch (...) {
        GlobalErrorStr() = L"hid_enumerate error: UNKNOWN";
    }
    return nullptr;
}

void HID_API_EXPORT HID_API_CALL hid_free_enumeration( //
    struct hid_device_info *d)
{
    while (d) {
        auto next = d->next;
        delete static_cast<HidDeviceInfoExt *>(d);
        d = next;
    }
}

HID_API_EXPORT hid_device *HID_API_CALL hid_open( //
    unsigned short vendor_id,
    unsigned short product_id,
    const wchar_t *serial_number)
{
    if (serial_number && serial_number[0] != L'\0') {
        return nullptr;
    }

    using namespace winrt;

    try {
        auto result = do_hid_open(vendor_id, product_id);
        if (result)
            GlobalErrorStr().clear();
        return result;
    } catch (const hresult_error &e) {
        GlobalErrorStr() = L"hid_open error: ";
        GlobalErrorStr() += e.message();
    } catch (const std::exception &e) {
        GlobalErrorStr() = L"hid_open error: ";
        GlobalErrorStr() += to_hstring(e.what());
    } catch (...) {
        GlobalErrorStr() = L"hid_open error: UNKNOWN";
    }
    return nullptr;
}

HID_API_EXPORT hid_device *HID_API_CALL hid_open_path( //
    const char *path)
{
    using namespace winrt;

    try {
        auto result = new hid_device_(to_hstring(path));
        if (result)
            GlobalErrorStr().clear();
        return result;
    } catch (const hresult_error &e) {
        GlobalErrorStr() = L"hid_open_path error: ";
        GlobalErrorStr() += e.message();
    } catch (const std::exception &e) {
        GlobalErrorStr() = L"hid_open_path error: ";
        GlobalErrorStr() += to_hstring(e.what());
    } catch (...) {
        GlobalErrorStr() = L"hid_open_path error: UNKNOWN";
    }
    return nullptr;
}

void HID_API_EXPORT HID_API_CALL hid_close( //
    hid_device *dev)
{
    delete dev;
}

int HID_API_EXPORT HID_API_CALL hid_write( //
    hid_device *dev,
    const unsigned char *data,
    size_t length)
{
    return -1;
}

int HID_API_EXPORT HID_API_CALL hid_read_timeout( //
    hid_device *dev,
    unsigned char *data,
    size_t length,
    int milliseconds)
{
    return -1;
}

int HID_API_EXPORT HID_API_CALL hid_read( //
    hid_device *dev,
    unsigned char *data,
    size_t length)
{
    return -1;
}

int HID_API_EXPORT HID_API_CALL hid_set_nonblocking( //
    hid_device *dev,
    int nonblock)
{
    return -1;
}

int HID_API_EXPORT HID_API_CALL hid_send_feature_report( //
    hid_device *dev,
    const unsigned char *data,
    size_t length)
{
    return -1;
}

int HID_API_EXPORT HID_API_CALL hid_get_feature_report( //
    hid_device *dev,
    unsigned char *data,
    size_t length)
{
    return -1;
}

int HID_API_EXPORT HID_API_CALL hid_get_input_report( //
    hid_device *dev,
    unsigned char *data,
    size_t length)
{
    return -1;
}

int HID_API_EXPORT_CALL hid_get_manufacturer_string( //
    hid_device *dev,
    wchar_t *string,
    size_t maxlen)
{
    return -1;
}

int HID_API_EXPORT_CALL hid_get_product_string( //
    hid_device *dev,
    wchar_t *string,
    size_t maxlen)
{
    return -1;
}

int HID_API_EXPORT_CALL hid_get_serial_number_string( //
    hid_device *dev,
    wchar_t *string,
    size_t maxlen)
{
    return -1;
}

struct hid_device_info HID_API_EXPORT *HID_API_CALL hid_get_device_info( //
    hid_device *dev)
{
    return dev->device_info;
}

int HID_API_EXPORT_CALL hid_get_indexed_string( //
    hid_device *dev,
    int string_index,
    wchar_t *string,
    size_t maxlen)
{
    return -1;
}

int HID_API_EXPORT_CALL hid_get_report_descriptor( //
    hid_device *dev,
    unsigned char *buf,
    size_t buf_size)
{
    return -1;
}

HID_API_EXPORT const wchar_t *HID_API_CALL hid_error( //
    hid_device *dev)
{
    if (dev) {
        if (dev->last_error_str.empty())
            return L"Success";
        return dev->last_error_str.c_str();
    }

    if (GlobalErrorStr().empty())
        return L"Success";
    return GlobalErrorStr().c_str();
}
