#include <hidapi_winrt.h>

#include <cwchar>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <winrt/base.h>

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.HumanInterfaceDevice.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>

namespace WinHid = winrt::Windows::Devices::HumanInterfaceDevice;
namespace WinStreams = winrt::Windows::Storage::Streams;
namespace WinEnumeration = winrt::Windows::Devices::Enumeration;

namespace {

struct HidDeviceInfoExt : public hid_device_info
{
    HidDeviceInfoExt()
        : hid_device_info{} // zero-initialized
    {
        interface_number = -1;
    }

    void FillFrom(const WinEnumeration::DeviceInformation &dev_info)
    {
        m_owned_path = to_string(dev_info.Id());
        path = m_owned_path.data();
        m_owned_product_string = dev_info.Name();
        product_string = const_cast<wchar_t *>(m_owned_product_string.c_str());
    }

    void FillFrom(const WinHid::HidDevice &dev)
    {
        vendor_id = dev.VendorId();
        product_id = dev.ProductId();
        release_number = dev.Version();
        usage_page = dev.UsagePage();
        usage = dev.UsageId();
    }

private:
    std::string m_owned_path;
    winrt::hstring m_owned_product_string;
};

} // namespace

// has to be defined in global namespace
struct hid_device_
{
    WinHid::HidDevice handle{nullptr};

    hid_device_(const WinEnumeration::DeviceInformation &dev_info)
        : hid_device_(dev_info.Id())
    {
        m_info = dev_info;
    }

    hid_device_(const winrt::hstring &id)
        : m_id(id)
        , m_info{nullptr}
    {
        handle = WinHid::HidDevice::FromIdAsync( //
                     id,
                     winrt::Windows::Storage::FileAccessMode::ReadWrite)
                     .get();
        if (!handle) {
            // ReadOnly fallback
            handle = WinHid::HidDevice::FromIdAsync( //
                         id,
                         winrt::Windows::Storage::FileAccessMode::Read)
                         .get();
        }

        if (!handle) {
            throw std::runtime_error("Failed to open device: " + winrt::to_string(id));
        }
    }

    ~hid_device_()
    {
        if (handle) {
            if (m_input_report_registration_token) {
                handle.InputReportReceived(m_input_report_registration_token);
            }

            handle.Close();
        }
    }

    void RegisterForInputReports()
    {
        m_input_report_registration_token = handle.InputReportReceived(
            {this, &hid_device::OnInputReport});
    }

    bool blocking = false;
    std::wstring last_error_str;

    auto &GetWinRTDeviceInformation()
    {
        if (!m_info) {
            m_info = WinEnumeration::DeviceInformation::CreateFromIdAsync(m_id).get();
        }

        if (!m_info) {
            throw std::runtime_error("Failed to get DeviceInformation from Device Instance ID");
        }

        return m_info;
    }

    HidDeviceInfoExt &GetInfo()
    {
        if (!m_device_info) {
            m_device_info.emplace();
            m_device_info->FillFrom(GetWinRTDeviceInformation());

            if (handle) {
                m_device_info->FillFrom(handle);
            }
        }

        return *m_device_info;
    }

    winrt::guid GetContainerId()
    {
        auto &info = GetWinRTDeviceInformation();
        return winrt::unbox_value<winrt::guid>(
            info.Properties().Lookup(L"System.Devices.ContainerId"));
    }

    size_t ReadInputReport(unsigned char *buf, size_t buf_size, int timeout_ms);

private:
    void OnInputReport(WinHid::HidDevice, WinHid::HidInputReportReceivedEventArgs args);

private:
    winrt::hstring m_id;
    WinEnumeration::DeviceInformation m_info;
    std::optional<HidDeviceInfoExt> m_device_info;
    winrt::event_token m_input_report_registration_token;

    std::mutex m_input_reports_mutex;
    std::condition_variable m_input_reports_condition;
    std::deque<std::vector<unsigned char>> m_input_reports;
};

namespace {

static constexpr hid_api_version api_version{
    HID_API_VERSION_MAJOR,
    HID_API_VERSION_MINOR,
    HID_API_VERSION_PATCH //
};

[[nodiscard]] static std::wstring &GlobalErrorStr()
{
    static std::wstring global_error;
    return global_error;
}

[[nodiscard]] auto WinRTHidEnumerate(unsigned short vendor_id, unsigned short product_id)
{
    std::wstring enumeration_selector = //
        L"System.Devices.InterfaceClassGuid:="
        "\"{4D1E55B2-F16F-11CF-88CB-001111000030}\" AND "
        L"System.Devices.InterfaceEnabled:=System.StructuredQueryType.Boolean#True";

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

[[nodiscard]] hid_device_info *do_hid_enumerate( //
    unsigned short vendor_id,
    unsigned short product_id)
{
    using namespace winrt;

    auto devices = WinRTHidEnumerate(vendor_id, product_id);

    std::unique_ptr<hid_device_info, decltype(&hid_free_enumeration)> result(nullptr,
                                                                             ::hid_free_enumeration);

    for (const auto &device : devices) {
        auto info = std::make_unique<HidDeviceInfoExt>();
        info->FillFrom(device);

        auto dev_h = WinHid::HidDevice::FromIdAsync(device.Id(),
                                                    winrt::Windows::Storage::FileAccessMode::Read)
                         .get();

        if (dev_h) {
            info->FillFrom(dev_h);
        }

        info->next = result.get();
        result.release();
        result.reset(info.release());
    }

    return result.release();
}

[[nodiscard]] std::unique_ptr<hid_device> do_hid_open( //
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

    return std::make_unique<hid_device>(devices.GetAt(0));
}

[[nodiscard]] WinStreams::IBuffer ToBuffer(const unsigned char *data, size_t data_size)
{
    // this is not the most efficient _possible_ way to create a n IBuffer,
    // (due to dynamic allocation, etc.)
    // but that is *the simplest* and recommended one:
    winrt::Windows::Storage::Streams::DataWriter writer;
    writer.WriteBytes({data, data + data_size});
    return writer.DetachBuffer();
}

[[nodiscard]] size_t FromBuffer(const WinStreams::IBuffer &buffer,
                                unsigned char *data,
                                size_t data_size)
{
    const size_t copy_size = std::min(size_t(buffer.Length()), data_size);
    auto reader = WinStreams::DataReader::FromBuffer(buffer);
    reader.ReadBytes({data, data + copy_size});
    return copy_size;
}

[[nodiscard]] std::vector<unsigned char> FromBuffer(const WinStreams::IBuffer &buffer)
{
    std::vector<unsigned char> result(buffer.Length());
    (void) FromBuffer(buffer, result.data(), result.size());
    return result;
}

[[nodiscard]] int WinRTHidSendFeatureReport( //
    WinHid::HidDevice &dev,
    uint16_t report_id,
    const WinStreams::IBuffer &report_buffer)
{
    auto report = dev.CreateFeatureReport(report_id);
    report.Data(report_buffer);

    return int(dev.SendFeatureReportAsync(report).get());
}

[[nodiscard]] int WinRTHidSendOutputReport( //
    WinHid::HidDevice &dev,
    uint16_t report_id,
    const WinStreams::IBuffer &report_buffer)
{
    auto report = dev.CreateOutputReport(report_id);
    report.Data(report_buffer);

    return int(dev.SendOutputReportAsync(report).get());
}

} // namespace

size_t hid_device_::ReadInputReport(unsigned char *buf, size_t buf_size, int timeout_ms)
{
    const auto now = std::chrono::steady_clock::now();
    std::unique_lock lck(m_input_reports_mutex);
    if (m_input_reports.empty()) {
        auto have_reports = [this]() { //
            return !m_input_reports.empty();
        };
        if (timeout_ms < 0) {
            m_input_reports_condition.wait(lck, have_reports);
        } else {
            m_input_reports_condition.wait_until( //
                lck,
                now + std::chrono::milliseconds(timeout_ms),
                have_reports);
        }
    }

    if (m_input_reports.empty()) {
        return 0;
    }

    auto report = m_input_reports.front();
    const size_t copy_size = std::min(report.size(), buf_size);
    std::memcpy(buf, report.data(), copy_size);
    m_input_reports.pop_front();
    return copy_size;
}

void hid_device_::OnInputReport(WinHid::HidDevice, WinHid::HidInputReportReceivedEventArgs args)
{
    auto report = args.Report();

    {
        const std::unique_lock lck(m_input_reports_mutex);
        m_input_reports.emplace_back(FromBuffer(report.Data()));

        constexpr size_t MaxReportsQueue = 64;
        if (m_input_reports.size() > MaxReportsQueue) {
            m_input_reports.pop_front();
        }
    }
    m_input_reports_condition.notify_all();
}

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

#define HIDAPI_CATCH_GLOBAL_EXCEPTION(ERR_PREFIX) \
    catch (const ::winrt::hresult_error &e) \
    { \
        GlobalErrorStr() = ERR_PREFIX " error: "; \
        GlobalErrorStr() += e.message(); \
    } \
    catch (const std::exception &e) \
    { \
        GlobalErrorStr() = ERR_PREFIX " error: "; \
        GlobalErrorStr() += ::winrt::to_hstring(e.what()); \
    } \
    catch (...) \
    { \
        GlobalErrorStr() = ERR_PREFIX " error: UNKNOWN"; \
    }

#define HIDAPI_CATCH_DEVICE_EXCEPTION(ERR_PREFIX) \
    catch (const ::winrt::hresult_error &e) \
    { \
        dev->last_error_str = ERR_PREFIX " error: "; \
        dev->last_error_str += e.message(); \
    } \
    catch (const std::exception &e) \
    { \
        dev->last_error_str = ERR_PREFIX " error: "; \
        dev->last_error_str += ::winrt::to_hstring(e.what()); \
    } \
    catch (...) \
    { \
        dev->last_error_str = ERR_PREFIX " error: UNKNOWN"; \
    }

struct hid_device_info HID_API_EXPORT *HID_API_CALL hid_enumerate( //
    unsigned short vendor_id,
    unsigned short product_id)
{
    using namespace winrt;

    try {
        auto result = do_hid_enumerate(vendor_id, product_id);
        if (result != nullptr)
            GlobalErrorStr().clear();
        return result;
    }
    HIDAPI_CATCH_GLOBAL_EXCEPTION(L"hid_enumerate")
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
    try {
        if (serial_number && serial_number[0] != L'\0') {
            GlobalErrorStr() = L"WinRT HIDAPI backand does not support filtering bu SerialNumber";
            return nullptr;
        }

        auto result = do_hid_open(vendor_id, product_id);
        if (result == nullptr) {
            // error already set
            return nullptr;
        }

        result->RegisterForInputReports();

        GlobalErrorStr().clear();
        return result.release();
    }
    HIDAPI_CATCH_GLOBAL_EXCEPTION(L"hid_open")
    return nullptr;
}

HID_API_EXPORT hid_device *HID_API_CALL hid_open_path( //
    const char *path)
{
    try {
        auto result = std::make_unique<hid_device>(winrt::to_hstring(path));
        result->RegisterForInputReports();

        GlobalErrorStr().clear();
        return result.release();
    }
    HIDAPI_CATCH_GLOBAL_EXCEPTION(L"hid_open_path")
    return nullptr;
}

void HID_API_EXPORT HID_API_CALL hid_close( //
    hid_device *dev)
{
    if (!dev)
        return;
    delete dev;
}

int HID_API_EXPORT HID_API_CALL hid_write( //
    hid_device *dev,
    const unsigned char *data,
    size_t length)
{
    try {
        if (!data || !length) {
            dev->last_error_str = L"Invalid buffer";
            return -1;
        }

        const int result = WinRTHidSendOutputReport(dev->handle, data[0], ToBuffer(data, length));

        if (result > 0) {
            dev->last_error_str.clear();
        }

        return result;
    }
    HIDAPI_CATCH_DEVICE_EXCEPTION(L"hid_write")
    return -1;
}

int HID_API_EXPORT HID_API_CALL hid_read_timeout( //
    hid_device *dev,
    unsigned char *data,
    size_t length,
    int milliseconds)
{
    try {
        if (!data || !length) {
            dev->last_error_str = L"Invalid buffer";
            return -1;
        }

        const size_t result = dev->ReadInputReport(data, length, milliseconds);

        if (result >= 0) {
            dev->last_error_str.clear();
        }

        return int(result);
    }
    HIDAPI_CATCH_DEVICE_EXCEPTION(L"hid_read_timeout")
    return -1;
}

int HID_API_EXPORT HID_API_CALL hid_read( //
    hid_device *dev,
    unsigned char *data,
    size_t length)
{
    return hid_read_timeout(dev, data, length, (dev->blocking) ? -1 : 0);
}

int HID_API_EXPORT HID_API_CALL hid_set_nonblocking( //
    hid_device *dev,
    int nonblock)
{
    return dev->blocking = (nonblock == 0);
}

int HID_API_EXPORT HID_API_CALL hid_send_feature_report( //
    hid_device *dev,
    const unsigned char *data,
    size_t length)
{
    try {
        if (!data || !length) {
            dev->last_error_str = L"Invalid buffer";
            return -1;
        }

        const int result = WinRTHidSendFeatureReport(dev->handle, data[0], ToBuffer(data, length));

        if (result > 0) {
            dev->last_error_str.clear();
        }

        return result;
    }
    HIDAPI_CATCH_DEVICE_EXCEPTION(L"hid_send_feature_report")
    return -1;
}

int HID_API_EXPORT HID_API_CALL hid_get_feature_report( //
    hid_device *dev,
    unsigned char *data,
    size_t length)
{
    try {
        if (!data || !length) {
            dev->last_error_str = L"Invalid buffer";
            return -1;
        }

        auto report = dev->handle.GetFeatureReportAsync(data[0]).get();

        if (!report) {
            dev->last_error_str = L"Failed to get Feature Report";
            return -1;
        }

        const size_t report_bytes = FromBuffer(report.Data(), data, length);

        if (report_bytes == 0) {
            dev->last_error_str = L"Got empty Feature report";
            return -1;
        } else {
            dev->last_error_str.clear();
        }

        return int(report_bytes);
    }
    HIDAPI_CATCH_DEVICE_EXCEPTION(L"hid_get_feature_report")
    return -1;
}

int HID_API_EXPORT HID_API_CALL hid_get_input_report( //
    hid_device *dev,
    unsigned char *data,
    size_t length)
{
    try {
        if (!data || !length) {
            dev->last_error_str = L"Invalid buffer";
            return -1;
        }

        auto report = dev->handle.GetInputReportAsync(data[0]).get();

        if (!report) {
            dev->last_error_str = L"Failed to get Input Report";
            return -1;
        }

        const size_t report_bytes = FromBuffer(report.Data(), data, length);

        if (report_bytes == 0) {
            dev->last_error_str = L"Got empty Input report";
            return -1;
        } else {
            dev->last_error_str.clear();
        }

        return int(report_bytes);
    }
    HIDAPI_CATCH_DEVICE_EXCEPTION(L"hid_get_input_report")
    return -1;
}

int HID_API_EXPORT_CALL hid_get_manufacturer_string( //
    hid_device *dev,
    wchar_t *string,
    size_t maxlen)
{
    if (!string || !maxlen) {
        dev->last_error_str = L"Invalid buffer";
        return -1;
    }

    auto info = hid_get_device_info(dev);
    if (!info) {
        // error already set
        return -1;
    }

    if (!info->manufacturer_string) {
        dev->last_error_str = L"Manufacturer String is not availale";
        return -1;
    }

    std::wcsncpy(string, info->manufacturer_string, maxlen);
    string[maxlen - 1] = L'\0';

    return 0;
}

int HID_API_EXPORT_CALL hid_get_product_string( //
    hid_device *dev,
    wchar_t *string,
    size_t maxlen)
{
    if (!string || !maxlen) {
        dev->last_error_str = L"Invalid buffer";
        return -1;
    }

    auto info = hid_get_device_info(dev);
    if (!info) {
        // error already set
        return -1;
    }

    if (!info->product_string) {
        dev->last_error_str = L"Product String is not availale";
        return -1;
    }

    std::wcsncpy(string, info->product_string, maxlen);
    string[maxlen - 1] = L'\0';

    return 0;
}

int HID_API_EXPORT_CALL hid_get_serial_number_string( //
    hid_device *dev,
    wchar_t *string,
    size_t maxlen)
{
    if (!string || !maxlen) {
        dev->last_error_str = L"Invalid buffer";
        return -1;
    }

    auto info = hid_get_device_info(dev);
    if (!info) {
        // error already set
        return -1;
    }

    if (!info->serial_number) {
        dev->last_error_str = L"Serial Number is not availale";
        return -1;
    }

    std::wcsncpy(string, info->serial_number, maxlen);
    string[maxlen - 1] = L'\0';

    return 0;
}

struct hid_device_info HID_API_EXPORT *HID_API_CALL hid_get_device_info( //
    hid_device *dev)
{
    try {
        return &dev->GetInfo();
    }
    HIDAPI_CATCH_DEVICE_EXCEPTION(L"hid_get_device_info")
    return nullptr;
}

int HID_API_EXPORT_CALL hid_get_indexed_string( //
    hid_device *dev,
    int /*string_index*/,
    wchar_t * /*string*/,
    size_t /*maxlen*/)
{
    dev->last_error_str = L"Not available for WinRT backend";
    return -1;
}

int HID_API_EXPORT_CALL hid_get_report_descriptor( //
    hid_device *dev,
    unsigned char * /*buf*/,
    size_t /*buf_size*/)
{
    dev->last_error_str = L"HID Report reconstruction is not implemnted for WinRT backend";

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

winrt::guid HID_API_EXPORT_CALL hid_winrt_get_container_id( //
    hid_device *dev)
{
    return dev->GetContainerId();
}
