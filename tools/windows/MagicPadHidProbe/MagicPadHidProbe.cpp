#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr USHORT kBluetoothAppleVendorId = 0x004c;
constexpr USHORT kMagicTrackpadUsbCProductId = 0x0324;
constexpr USHORT kRewrittenDriverVendorId = 0x8910;

class DeviceInfoSet {
public:
    explicit DeviceInfoSet(HDEVINFO value) : value_(value) {}
    ~DeviceInfoSet()
    {
        if (value_ != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(value_);
        }
    }

    DeviceInfoSet(const DeviceInfoSet&) = delete;
    DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;

    HDEVINFO get() const { return value_; }

private:
    HDEVINFO value_;
};

class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle()
    {
        if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) {
            CloseHandle(value_);
        }
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    HANDLE get() const { return value_; }
    bool valid() const { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }

private:
    HANDLE value_;
};

class PreparsedData {
public:
    PreparsedData() = default;
    ~PreparsedData()
    {
        if (value_ != nullptr) {
            HidD_FreePreparsedData(value_);
        }
    }

    PreparsedData(const PreparsedData&) = delete;
    PreparsedData& operator=(const PreparsedData&) = delete;

    PHIDP_PREPARSED_DATA* receive() { return &value_; }
    PHIDP_PREPARSED_DATA get() const { return value_; }

private:
    PHIDP_PREPARSED_DATA value_ = nullptr;
};

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed");
    }

    std::string result(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            required,
            nullptr,
            nullptr) != required) {
        throw std::runtime_error("WideCharToMultiByte returned a short result");
    }
    return result;
}

std::wstring Uppercase(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(towupper(character));
    });
    return value;
}

bool ContainsCaseInsensitive(const std::wstring& value, const std::wstring& expected)
{
    return Uppercase(value).find(Uppercase(expected)) != std::wstring::npos;
}

std::string JsonEscape(const std::string& value)
{
    std::ostringstream output;
    for (unsigned char character : value) {
        switch (character) {
        case '\"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned int>(character) << std::dec;
            }
            else {
                output << static_cast<char>(character);
            }
            break;
        }
    }
    return output.str();
}

void WriteJsonString(std::ostream& output, const std::string& value)
{
    output << '\"' << JsonEscape(value) << '\"';
}

void WriteJsonWideString(std::ostream& output, const std::wstring& value)
{
    WriteJsonString(output, WideToUtf8(value));
}

std::string Hex16(USHORT value)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(4) << std::setfill('0')
           << static_cast<unsigned int>(value);
    return output.str();
}

std::string Hex32(ULONG value)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(8) << std::setfill('0')
           << static_cast<unsigned long>(value);
    return output.str();
}

std::wstring GetDeviceInstanceId(HDEVINFO informationSet, SP_DEVINFO_DATA* deviceInfo)
{
    DWORD required = 0;
    SetupDiGetDeviceInstanceIdW(informationSet, deviceInfo, nullptr, 0, &required);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) {
        return {};
    }

    std::vector<wchar_t> buffer(required);
    if (!SetupDiGetDeviceInstanceIdW(
            informationSet,
            deviceInfo,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            nullptr)) {
        return {};
    }
    return std::wstring(buffer.data());
}

const char* ReportTypeName(HIDP_REPORT_TYPE reportType)
{
    switch (reportType) {
    case HidP_Input:
        return "input";
    case HidP_Output:
        return "output";
    case HidP_Feature:
        return "feature";
    default:
        return "unknown";
    }
}

USHORT ButtonCapsCount(const HIDP_CAPS& caps, HIDP_REPORT_TYPE reportType)
{
    switch (reportType) {
    case HidP_Input:
        return caps.NumberInputButtonCaps;
    case HidP_Output:
        return caps.NumberOutputButtonCaps;
    case HidP_Feature:
        return caps.NumberFeatureButtonCaps;
    default:
        return 0;
    }
}

USHORT ValueCapsCount(const HIDP_CAPS& caps, HIDP_REPORT_TYPE reportType)
{
    switch (reportType) {
    case HidP_Input:
        return caps.NumberInputValueCaps;
    case HidP_Output:
        return caps.NumberOutputValueCaps;
    case HidP_Feature:
        return caps.NumberFeatureValueCaps;
    default:
        return 0;
    }
}

void WriteButtonCap(std::ostream& output, const HIDP_BUTTON_CAPS& cap)
{
    output << "{";
    output << "\"usagePage\":" << cap.UsagePage << ",";
    output << "\"reportId\":" << static_cast<unsigned int>(cap.ReportID) << ",";
    output << "\"isAlias\":" << (cap.IsAlias ? "true" : "false") << ",";
    output << "\"bitField\":" << cap.BitField << ",";
    output << "\"linkCollection\":" << cap.LinkCollection << ",";
    output << "\"linkUsagePage\":" << cap.LinkUsagePage << ",";
    output << "\"linkUsage\":" << cap.LinkUsage << ",";
    output << "\"isRange\":" << (cap.IsRange ? "true" : "false") << ",";
    output << "\"isAbsolute\":" << (cap.IsAbsolute ? "true" : "false") << ",";
    if (cap.IsRange) {
        output << "\"usageMin\":" << cap.Range.UsageMin << ",";
        output << "\"usageMax\":" << cap.Range.UsageMax << ",";
        output << "\"dataIndexMin\":" << cap.Range.DataIndexMin << ",";
        output << "\"dataIndexMax\":" << cap.Range.DataIndexMax;
    }
    else {
        output << "\"usage\":" << cap.NotRange.Usage << ",";
        output << "\"dataIndex\":" << cap.NotRange.DataIndex;
    }
    output << "}";
}

void WriteValueCap(std::ostream& output, const HIDP_VALUE_CAPS& cap)
{
    output << "{";
    output << "\"usagePage\":" << cap.UsagePage << ",";
    output << "\"reportId\":" << static_cast<unsigned int>(cap.ReportID) << ",";
    output << "\"isAlias\":" << (cap.IsAlias ? "true" : "false") << ",";
    output << "\"bitField\":" << cap.BitField << ",";
    output << "\"linkCollection\":" << cap.LinkCollection << ",";
    output << "\"linkUsagePage\":" << cap.LinkUsagePage << ",";
    output << "\"linkUsage\":" << cap.LinkUsage << ",";
    output << "\"isRange\":" << (cap.IsRange ? "true" : "false") << ",";
    output << "\"isAbsolute\":" << (cap.IsAbsolute ? "true" : "false") << ",";
    output << "\"hasNull\":" << (cap.HasNull ? "true" : "false") << ",";
    output << "\"bitSize\":" << cap.BitSize << ",";
    output << "\"reportCount\":" << cap.ReportCount << ",";
    output << "\"unitsExponent\":" << cap.UnitsExp << ",";
    output << "\"units\":" << cap.Units << ",";
    output << "\"logicalMin\":" << cap.LogicalMin << ",";
    output << "\"logicalMax\":" << cap.LogicalMax << ",";
    output << "\"physicalMin\":" << cap.PhysicalMin << ",";
    output << "\"physicalMax\":" << cap.PhysicalMax << ",";
    if (cap.IsRange) {
        output << "\"usageMin\":" << cap.Range.UsageMin << ",";
        output << "\"usageMax\":" << cap.Range.UsageMax << ",";
        output << "\"dataIndexMin\":" << cap.Range.DataIndexMin << ",";
        output << "\"dataIndexMax\":" << cap.Range.DataIndexMax;
    }
    else {
        output << "\"usage\":" << cap.NotRange.Usage << ",";
        output << "\"dataIndex\":" << cap.NotRange.DataIndex;
    }
    output << "}";
}

bool WriteButtonCaps(
    std::ostream& output,
    HIDP_REPORT_TYPE reportType,
    const HIDP_CAPS& caps,
    PHIDP_PREPARSED_DATA preparsedData)
{
    USHORT requested = ButtonCapsCount(caps, reportType);
    std::vector<HIDP_BUTTON_CAPS> values(requested);
    USHORT returned = requested;
    const NTSTATUS status = requested == 0
        ? HIDP_STATUS_SUCCESS
        : HidP_GetButtonCaps(reportType, values.data(), &returned, preparsedData);

    output << "\"" << ReportTypeName(reportType) << "\":{";
    output << "\"status\":";
    WriteJsonString(output, Hex32(static_cast<ULONG>(status)));
    output << ",\"caps\":[";
    if (status == HIDP_STATUS_SUCCESS) {
        for (USHORT index = 0; index < returned; ++index) {
            if (index != 0) {
                output << ",";
            }
            WriteButtonCap(output, values[index]);
        }
    }
    output << "]}";
    return status == HIDP_STATUS_SUCCESS;
}

bool WriteValueCaps(
    std::ostream& output,
    HIDP_REPORT_TYPE reportType,
    const HIDP_CAPS& caps,
    PHIDP_PREPARSED_DATA preparsedData)
{
    USHORT requested = ValueCapsCount(caps, reportType);
    std::vector<HIDP_VALUE_CAPS> values(requested);
    USHORT returned = requested;
    const NTSTATUS status = requested == 0
        ? HIDP_STATUS_SUCCESS
        : HidP_GetValueCaps(reportType, values.data(), &returned, preparsedData);

    output << "\"" << ReportTypeName(reportType) << "\":{";
    output << "\"status\":";
    WriteJsonString(output, Hex32(static_cast<ULONG>(status)));
    output << ",\"caps\":[";
    if (status == HIDP_STATUS_SUCCESS) {
        for (USHORT index = 0; index < returned; ++index) {
            if (index != 0) {
                output << ",";
            }
            WriteValueCap(output, values[index]);
        }
    }
    output << "]}";
    return status == HIDP_STATUS_SUCCESS;
}

bool WriteLinkCollectionNodes(
    std::ostream& output,
    const HIDP_CAPS& caps,
    PHIDP_PREPARSED_DATA preparsedData)
{
    ULONG requested = caps.NumberLinkCollectionNodes;
    std::vector<HIDP_LINK_COLLECTION_NODE> nodes(requested);
    ULONG returned = requested;
    const NTSTATUS status = requested == 0
        ? HIDP_STATUS_SUCCESS
        : HidP_GetLinkCollectionNodes(nodes.data(), &returned, preparsedData);

    output << "\"linkCollectionNodes\":{";
    output << "\"status\":";
    WriteJsonString(output, Hex32(static_cast<ULONG>(status)));
    output << ",\"nodes\":[";
    if (status == HIDP_STATUS_SUCCESS) {
        for (ULONG index = 0; index < returned; ++index) {
            if (index != 0) {
                output << ",";
            }
            const HIDP_LINK_COLLECTION_NODE& node = nodes[index];
            output << "{";
            output << "\"index\":" << index << ",";
            output << "\"linkUsagePage\":" << node.LinkUsagePage << ",";
            output << "\"linkUsage\":" << node.LinkUsage << ",";
            output << "\"parent\":" << node.Parent << ",";
            output << "\"numberOfChildren\":" << node.NumberOfChildren << ",";
            output << "\"nextSibling\":" << node.NextSibling << ",";
            output << "\"firstChild\":" << node.FirstChild << ",";
            output << "\"collectionType\":" << node.CollectionType << ",";
            output << "\"isAlias\":" << (node.IsAlias ? "true" : "false");
            output << "}";
        }
    }
    output << "]}";
    return status == HIDP_STATUS_SUCCESS;
}

struct InterfaceRecord {
    std::wstring interfacePath;
    std::wstring instanceId;
    std::string collection;
    bool exactPnpMatch = false;
    bool physicalAttributeMatch = false;
    bool rewrittenAttributeMatch = false;
    DWORD openError = ERROR_SUCCESS;
    bool attributesAvailable = false;
    HIDD_ATTRIBUTES attributes{};
    bool preparsedDataAvailable = false;
    DWORD preparsedDataError = ERROR_SUCCESS;
    NTSTATUS capsStatus = HIDP_STATUS_INTERNAL_ERROR;
    HIDP_CAPS caps{};
    bool descriptorCapsComplete = false;
    std::string descriptorCapsJson;
};

std::string DetectCollection(
    const std::wstring& interfacePath,
    const std::wstring& instanceId)
{
    const std::wstring value = Uppercase(interfacePath + L" " + instanceId);
    size_t position = value.find(L"COL");
    while (position != std::wstring::npos) {
        if (position + 5 <= value.size() &&
            iswxdigit(value[position + 3]) &&
            iswxdigit(value[position + 4])) {
            return "Col" + WideToUtf8(value.substr(position + 3, 2));
        }
        position = value.find(L"COL", position + 3);
    }
    return "unknown";
}

bool HasTargetPidHint(const std::wstring& interfacePath, const std::wstring& instanceId)
{
    return ContainsCaseInsensitive(interfacePath, L"PID&0324") ||
        ContainsCaseInsensitive(interfacePath, L"PID_0324") ||
        ContainsCaseInsensitive(instanceId, L"PID&0324") ||
        ContainsCaseInsensitive(instanceId, L"PID_0324");
}

bool HasExactPhysicalPnpId(const std::wstring& interfacePath, const std::wstring& instanceId)
{
    constexpr wchar_t kBluetoothId[] = L"VID&0001004C_PID&0324";
    return ContainsCaseInsensitive(interfacePath, kBluetoothId) ||
        ContainsCaseInsensitive(instanceId, kBluetoothId);
}

InterfaceRecord ProbeInterface(const std::wstring& interfacePath, const std::wstring& instanceId)
{
    InterfaceRecord record;
    record.interfacePath = interfacePath;
    record.instanceId = instanceId;
    record.collection = DetectCollection(interfacePath, instanceId);
    record.exactPnpMatch = HasExactPhysicalPnpId(interfacePath, instanceId);

    // This is intentionally a metadata-only handle. Do not add GENERIC_READ or
    // GENERIC_WRITE: mouse and PTP collections are normally owned exclusively by
    // the Windows input stack, and this probe must never consume input reports.
    Handle handle(CreateFileW(
        interfacePath.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!handle.valid()) {
        record.openError = GetLastError();
        return record;
    }

    record.attributes.Size = sizeof(record.attributes);
    if (HidD_GetAttributes(handle.get(), &record.attributes)) {
        record.attributesAvailable = true;
        record.physicalAttributeMatch =
            record.attributes.VendorID == kBluetoothAppleVendorId &&
            record.attributes.ProductID == kMagicTrackpadUsbCProductId;
        record.rewrittenAttributeMatch =
            record.attributes.VendorID == kRewrittenDriverVendorId &&
            record.attributes.ProductID == kMagicTrackpadUsbCProductId;
    }

    PreparsedData preparsedData;
    if (!HidD_GetPreparsedData(handle.get(), preparsedData.receive())) {
        record.preparsedDataError = GetLastError();
        return record;
    }
    record.preparsedDataAvailable = true;

    record.capsStatus = HidP_GetCaps(preparsedData.get(), &record.caps);
    if (record.capsStatus != HIDP_STATUS_SUCCESS) {
        return record;
    }

    std::ostringstream descriptorCaps;
    bool descriptorCapsComplete = true;
    descriptorCaps << "{";
    descriptorCaps << "\"buttonCaps\":{";
    descriptorCapsComplete &= WriteButtonCaps(
        descriptorCaps, HidP_Input, record.caps, preparsedData.get());
    descriptorCaps << ",";
    descriptorCapsComplete &= WriteButtonCaps(
        descriptorCaps, HidP_Output, record.caps, preparsedData.get());
    descriptorCaps << ",";
    descriptorCapsComplete &= WriteButtonCaps(
        descriptorCaps, HidP_Feature, record.caps, preparsedData.get());
    descriptorCaps << "},\"valueCaps\":{";
    descriptorCapsComplete &= WriteValueCaps(
        descriptorCaps, HidP_Input, record.caps, preparsedData.get());
    descriptorCaps << ",";
    descriptorCapsComplete &= WriteValueCaps(
        descriptorCaps, HidP_Output, record.caps, preparsedData.get());
    descriptorCaps << ",";
    descriptorCapsComplete &= WriteValueCaps(
        descriptorCaps, HidP_Feature, record.caps, preparsedData.get());
    descriptorCaps << "},";
    descriptorCapsComplete &= WriteLinkCollectionNodes(
        descriptorCaps, record.caps, preparsedData.get());
    descriptorCaps << "}";
    record.descriptorCapsComplete = descriptorCapsComplete;
    record.descriptorCapsJson = descriptorCaps.str();
    return record;
}

bool IsTargetRecord(const InterfaceRecord& record)
{
    return record.exactPnpMatch ||
        record.physicalAttributeMatch ||
        record.rewrittenAttributeMatch;
}

void WriteInterfaceRecord(std::ostream& output, const InterfaceRecord& record)
{
    output << "{";
    output << "\"collection\":";
    WriteJsonString(output, record.collection);
    output << ",";
    output << "\"interfacePath\":";
    WriteJsonWideString(output, record.interfacePath);
    output << ",\"instanceId\":";
    WriteJsonWideString(output, record.instanceId);
    output << ",\"match\":{";
    output << "\"exactPhysicalPnpId\":" << (record.exactPnpMatch ? "true" : "false") << ",";
    output << "\"physicalAttributes\":" << (record.physicalAttributeMatch ? "true" : "false") << ",";
    output << "\"rewrittenDriverAttributes\":" << (record.rewrittenAttributeMatch ? "true" : "false");
    output << "},\"open\":{";
    output << "\"desiredAccess\":0,";
    output << "\"succeeded\":" << (record.openError == ERROR_SUCCESS ? "true" : "false") << ",";
    output << "\"win32Error\":" << record.openError;
    output << "},\"attributes\":{";
    output << "\"available\":" << (record.attributesAvailable ? "true" : "false");
    if (record.attributesAvailable) {
        output << ",\"vendorId\":";
        WriteJsonString(output, Hex16(record.attributes.VendorID));
        output << ",\"productId\":";
        WriteJsonString(output, Hex16(record.attributes.ProductID));
        output << ",\"versionNumber\":";
        WriteJsonString(output, Hex16(record.attributes.VersionNumber));
    }
    output << "},\"preparsedData\":{";
    output << "\"available\":" << (record.preparsedDataAvailable ? "true" : "false") << ",";
    output << "\"win32Error\":" << record.preparsedDataError;
    output << "},\"caps\":{";
    output << "\"status\":";
    WriteJsonString(output, Hex32(static_cast<ULONG>(record.capsStatus)));
    if (record.capsStatus == HIDP_STATUS_SUCCESS) {
        output << ",\"usagePage\":" << record.caps.UsagePage;
        output << ",\"usage\":" << record.caps.Usage;
        output << ",\"inputReportByteLength\":" << record.caps.InputReportByteLength;
        output << ",\"outputReportByteLength\":" << record.caps.OutputReportByteLength;
        output << ",\"featureReportByteLength\":" << record.caps.FeatureReportByteLength;
        output << ",\"numberLinkCollectionNodes\":" << record.caps.NumberLinkCollectionNodes;
        output << ",\"numberInputButtonCaps\":" << record.caps.NumberInputButtonCaps;
        output << ",\"numberInputValueCaps\":" << record.caps.NumberInputValueCaps;
        output << ",\"numberInputDataIndices\":" << record.caps.NumberInputDataIndices;
        output << ",\"numberOutputButtonCaps\":" << record.caps.NumberOutputButtonCaps;
        output << ",\"numberOutputValueCaps\":" << record.caps.NumberOutputValueCaps;
        output << ",\"numberOutputDataIndices\":" << record.caps.NumberOutputDataIndices;
        output << ",\"numberFeatureButtonCaps\":" << record.caps.NumberFeatureButtonCaps;
        output << ",\"numberFeatureValueCaps\":" << record.caps.NumberFeatureValueCaps;
        output << ",\"numberFeatureDataIndices\":" << record.caps.NumberFeatureDataIndices;
    }
    output << "}";
    if (!record.descriptorCapsJson.empty()) {
        output << ",\"descriptorCapsComplete\":"
               << (record.descriptorCapsComplete ? "true" : "false");
        output << ",\"descriptorVisibleCaps\":" << record.descriptorCapsJson;
    }
    output << "}";
}

std::vector<InterfaceRecord> EnumerateTargetInterfaces()
{
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);

    DeviceInfoSet informationSet(SetupDiGetClassDevsW(
        &hidGuid,
        nullptr,
        nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (informationSet.get() == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("SetupDiGetClassDevsW failed");
    }

    std::vector<InterfaceRecord> records;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(
                informationSet.get(),
                nullptr,
                &hidGuid,
                index,
                &interfaceData)) {
            const DWORD error = GetLastError();
            if (error == ERROR_NO_MORE_ITEMS) {
                break;
            }
            throw std::runtime_error("SetupDiEnumDeviceInterfaces failed");
        }

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(
            informationSet.get(),
            &interfaceData,
            nullptr,
            0,
            &required,
            nullptr);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) {
            continue;
        }

        std::vector<unsigned char> detailBuffer(required, 0);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA deviceInfo{};
        deviceInfo.cbSize = sizeof(deviceInfo);
        if (!SetupDiGetDeviceInterfaceDetailW(
                informationSet.get(),
                &interfaceData,
                detail,
                required,
                nullptr,
                &deviceInfo)) {
            continue;
        }

        const std::wstring interfacePath(detail->DevicePath);
        const std::wstring instanceId = GetDeviceInstanceId(informationSet.get(), &deviceInfo);
        if (!HasTargetPidHint(interfacePath, instanceId)) {
            continue;
        }

        InterfaceRecord record = ProbeInterface(interfacePath, instanceId);
        if (IsTargetRecord(record)) {
            records.push_back(std::move(record));
        }
    }
    return records;
}

void WriteDocument(std::ostream& output, const std::vector<InterfaceRecord>& records)
{
    const bool capsAvailable = std::any_of(
        records.begin(),
        records.end(),
        [](const InterfaceRecord& record) {
            return record.capsStatus == HIDP_STATUS_SUCCESS;
        });
    const bool allCapsComplete = !records.empty() && std::all_of(
        records.begin(),
        records.end(),
        [](const InterfaceRecord& record) {
            return record.capsStatus == HIDP_STATUS_SUCCESS &&
                record.descriptorCapsComplete;
        });

    output << "{";
    output << "\"schema\":\"magicpad-hid-probe/v1\",";
    output << "\"target\":{";
    output << "\"transport\":\"bluetooth\",";
    output << "\"physicalVendorId\":\"0x004c\",";
    output << "\"productId\":\"0x0324\",";
    output << "\"rewrittenDriverVendorId\":\"0x8910\"";
    output << "},";
    output << "\"safety\":{";
    output << "\"createFileDesiredAccess\":0,";
    output << "\"reportIoAttempted\":false,";
    output << "\"serialCollected\":false";
    output << "},";
    output << "\"status\":";
    WriteJsonString(
        output,
        records.empty() ? "targetNotFound" :
        allCapsComplete ? "ok" :
        capsAvailable ? "partial" : "capsUnavailable");
    output << ",\"devices\":[";
    for (size_t index = 0; index < records.size(); ++index) {
        if (index != 0) {
            output << ",";
        }
        WriteInterfaceRecord(output, records[index]);
    }
    output << "]}" << std::endl;
}

} // namespace

int wmain()
{
    try {
        WriteDocument(std::cout, EnumerateTargetInterfaces());
        return 0;
    }
    catch (const std::exception& error) {
        std::cout << "{\"schema\":\"magicpad-hid-probe/v1\","
                     "\"status\":\"probeFailed\",\"error\":";
        WriteJsonString(std::cout, error.what());
        std::cout << "}" << std::endl;
        return 1;
    }
}
