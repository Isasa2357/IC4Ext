#include <ic4/ic4.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

class IC4LibraryGuard
{
public:
    IC4LibraryGuard()
    {
        ic4::InitLibraryConfig config;
        config.defaultErrorHandlerBehavior = ic4::ErrorHandlerBehavior::Ignore;
        initialized_ = ic4::initLibrary(config);
    }

    ~IC4LibraryGuard()
    {
        if (initialized_) ic4::exitLibrary();
    }

    bool initialized() const noexcept { return initialized_; }

private:
    bool initialized_ = false;
};

const char* ArgValue(int argc, char** argv, const char* name)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == name) return argv[i + 1];
    }
    return nullptr;
}

bool HasFlag(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == name) return true;
    }
    return false;
}

std::string ErrorText(const ic4::Error& err)
{
    if (!err.isError()) return {};
    return "code=" + std::to_string(static_cast<int>(err.code())) + " message=" + err.message();
}

std::string TransportTypeName(ic4::TransportLayerType type)
{
    switch (type) {
    case ic4::TransportLayerType::USB3Vision: return "USB3Vision";
    case ic4::TransportLayerType::GigEVision: return "GigEVision";
    default: return "Unknown";
    }
}

std::string SafeDeviceString(const ic4::DeviceInfo& device,
                             const char* label,
                             std::string (ic4::DeviceInfo::*getter)(ic4::Error&) const)
{
    ic4::Error err;
    const std::string value = (device.*getter)(err);
    if (err.isError()) {
        return std::string("<") + label + " error: " + ErrorText(err) + ">";
    }
    return value;
}

void PrintDevice(std::size_t index, const ic4::DeviceInfo& device)
{
    std::cout << "[" << index << "]\n";
    std::cout << "  model       : " << SafeDeviceString(device, "model", &ic4::DeviceInfo::modelName) << "\n";
    std::cout << "  serial      : " << SafeDeviceString(device, "serial", &ic4::DeviceInfo::serial) << "\n";
    std::cout << "  uniqueName  : " << SafeDeviceString(device, "uniqueName", &ic4::DeviceInfo::uniqueName) << "\n";
    std::cout << "  version     : " << SafeDeviceString(device, "version", &ic4::DeviceInfo::version) << "\n";
    std::cout << "  userID      : " << SafeDeviceString(device, "userID", &ic4::DeviceInfo::userID) << "\n";

    ic4::Error interfaceError;
    const auto interface = device.getInterface(interfaceError);
    if (interfaceError.isError() || !interface) {
        std::cout << "  interface   : <error: " << ErrorText(interfaceError) << ">\n";
        return;
    }

    {
        ic4::Error err;
        const auto value = interface.interfaceDisplayName(err);
        std::cout << "  interface   : "
                  << (err.isError() ? std::string("<error: ") + ErrorText(err) + ">" : value)
                  << "\n";
    }
    {
        ic4::Error err;
        const auto value = interface.transportLayerName(err);
        std::cout << "  tlName      : "
                  << (err.isError() ? std::string("<error: ") + ErrorText(err) + ">" : value)
                  << "\n";
    }
    {
        ic4::Error err;
        const auto value = interface.transportLayerVersion(err);
        std::cout << "  tlVersion   : "
                  << (err.isError() ? std::string("<error: ") + ErrorText(err) + ">" : value)
                  << "\n";
    }
    {
        ic4::Error err;
        const auto value = interface.transportLayerType(err);
        std::cout << "  tlType      : "
                  << (err.isError() ? std::string("<error: ") + ErrorText(err) + ">" : TransportTypeName(value))
                  << "\n";
    }
}

void PrintInteger(ic4::PropertyMap& properties,
                  const ic4::PropId::PropIdInteger& id,
                  const char* name,
                  bool& allCriticalReadsSucceeded,
                  bool critical = false)
{
    ic4::Error err;
    const auto property = properties.find(id, err);
    if (err.isError() || !property) {
        std::cout << "  " << name << " : <unavailable";
        if (err.isError()) std::cout << ": " << ErrorText(err);
        std::cout << ">\n";
        if (critical) allCriticalReadsSucceeded = false;
        return;
    }

    const auto value = property.getValue(err);
    if (err.isError()) {
        std::cout << "  " << name << " : <read error: " << ErrorText(err) << ">\n";
        if (critical) allCriticalReadsSucceeded = false;
        return;
    }
    std::cout << "  " << name << " : " << value << "\n";
}

void PrintFloat(ic4::PropertyMap& properties,
                const ic4::PropId::PropIdFloat& id,
                const char* name)
{
    ic4::Error err;
    const auto property = properties.find(id, err);
    if (err.isError() || !property) {
        std::cout << "  " << name << " : <unavailable";
        if (err.isError()) std::cout << ": " << ErrorText(err);
        std::cout << ">\n";
        return;
    }

    const auto value = property.getValue(err);
    if (err.isError()) {
        std::cout << "  " << name << " : <read error: " << ErrorText(err) << ">\n";
        return;
    }
    std::cout << "  " << name << " : " << value << "\n";
}

void PrintEnumeration(ic4::PropertyMap& properties,
                      const ic4::PropId::PropIdEnumeration& id,
                      const char* name)
{
    ic4::Error err;
    const auto property = properties.find(id, err);
    if (err.isError() || !property) {
        std::cout << "  " << name << " : <unavailable";
        if (err.isError()) std::cout << ": " << ErrorText(err);
        std::cout << ">\n";
        return;
    }

    const auto value = property.getValue(err);
    if (err.isError()) {
        std::cout << "  " << name << " : <read error: " << ErrorText(err) << ">\n";
        return;
    }
    std::cout << "  " << name << " : " << value << "\n";
}

bool LoadDefaultUserSet(ic4::Grabber& grabber)
{
    ic4::Error mapError;
    auto properties = grabber.devicePropertyMap(mapError);
    if (mapError.isError() || !properties) {
        std::cerr << "Default UserSet: devicePropertyMap failed: " << ErrorText(mapError) << "\n";
        return false;
    }

    ic4::Error selectorError;
    if (!properties.setValue(ic4::PropId::UserSetSelector, "Default", selectorError)) {
        std::cerr << "Default UserSet: UserSetSelector=Default failed: "
                  << ErrorText(selectorError) << "\n";
        return false;
    }

    ic4::Error loadError;
    if (!properties.executeCommand(ic4::PropId::UserSetLoad, loadError)) {
        std::cerr << "Default UserSet: UserSetLoad failed: " << ErrorText(loadError) << "\n";
        return false;
    }

    std::cout << "Default UserSet: loaded successfully\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return true;
}

bool ProbeStream(ic4::Grabber& grabber, int waitMs)
{
    std::atomic<std::uint64_t> frameCount{0};

    ic4::Error sinkError;
    auto sink = ic4::QueueSink::create(
        [&frameCount](ic4::QueueSink& queueSink) {
            for (;;) {
                ic4::Error popError;
                auto buffer = queueSink.popOutputBuffer(popError);
                if (popError.isError() || !buffer) break;
                ++frameCount;
            }
        },
        sinkError);

    if (sinkError.isError() || !sink) {
        std::cerr << "Official-style stream probe: QueueSink::create failed: "
                  << ErrorText(sinkError) << "\n";
        return false;
    }

    ic4::Error setupError;
    if (!grabber.streamSetup(sink, ic4::StreamSetupOption::AcquisitionStart, setupError)) {
        std::cerr << "Official-style stream probe: streamSetup failed: "
                  << ErrorText(setupError) << "\n";
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    while (frameCount.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ic4::Error stopError;
    grabber.streamStop(stopError);
    if (stopError.isError()) {
        std::cerr << "Official-style stream probe: streamStop warning: "
                  << ErrorText(stopError) << "\n";
    }

    std::cout << "Official-style stream probe: framesReceived=" << frameCount.load() << "\n";
    if (frameCount.load() == 0) {
        std::cerr << "Official-style stream probe: stream opened but no frame arrived within "
                  << waitMs << " ms\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    IC4LibraryGuard library;
    if (!library.initialized()) {
        std::cerr << "ic4::initLibrary failed\n";
        return 1;
    }

    ic4::Error enumError;
    const auto devices = ic4::DeviceEnum::enumDevices(enumError);
    if (enumError.isError()) {
        std::cerr << "Device enumeration failed: " << ErrorText(enumError) << "\n";
        return 1;
    }

    std::cout << "IC4 devices found: " << devices.size() << "\n";
    for (std::size_t i = 0; i < devices.size(); ++i) {
        PrintDevice(i, devices[i]);
    }

    const char* indexText = ArgValue(argc, argv, "--device-index");
    const char* serialText = ArgValue(argc, argv, "--serial");
    const bool loadDefaultUserSet = HasFlag(argc, argv, "--load-default-userset");
    const bool probeStream = HasFlag(argc, argv, "--probe-stream");
    int streamWaitMs = 3000;
    if (const char* value = ArgValue(argc, argv, "--stream-wait-ms")) {
        streamWaitMs = std::max(1, std::atoi(value));
    }

    if (!indexText && !serialText) {
        std::cout << "\nSpecify --device-index N or --serial SERIAL to open and probe one device.\n";
        return 0;
    }

    std::optional<std::size_t> selectedIndex;
    if (serialText) {
        for (std::size_t i = 0; i < devices.size(); ++i) {
            ic4::Error serialError;
            if (devices[i].serial(serialError) == serialText && !serialError.isError()) {
                selectedIndex = i;
                break;
            }
        }
        if (!selectedIndex) {
            std::cerr << "No device with serial " << serialText << "\n";
            return 1;
        }
    } else {
        const int index = std::atoi(indexText);
        if (index < 0 || static_cast<std::size_t>(index) >= devices.size()) {
            std::cerr << "--device-index is out of range\n";
            return 1;
        }
        selectedIndex = static_cast<std::size_t>(index);
    }

    std::cout << "\nOpening selected device index " << *selectedIndex << "\n";
    PrintDevice(*selectedIndex, devices[*selectedIndex]);

    ic4::Error grabberError;
    ic4::Grabber grabber(grabberError);
    if (grabberError.isError() || !grabber) {
        std::cerr << "Grabber creation failed: " << ErrorText(grabberError) << "\n";
        return 1;
    }

    if (!grabber.deviceOpen(devices[*selectedIndex], grabberError)) {
        std::cerr << "deviceOpen failed: " << ErrorText(grabberError) << "\n";
        return 1;
    }
    std::cout << "deviceOpen: success\n";

    if (loadDefaultUserSet && !LoadDefaultUserSet(grabber)) {
        return 3;
    }

    auto properties = grabber.devicePropertyMap(grabberError);
    if (grabberError.isError() || !properties) {
        std::cerr << "devicePropertyMap failed: " << ErrorText(grabberError) << "\n";
        return 1;
    }

    bool criticalReadsSucceeded = true;
    std::cout << "Device properties:\n";
    PrintInteger(properties, ic4::PropId::Width, "Width", criticalReadsSucceeded);
    PrintInteger(properties, ic4::PropId::Height, "Height", criticalReadsSucceeded);
    PrintEnumeration(properties, ic4::PropId::PixelFormat, "PixelFormat");
    PrintFloat(properties, ic4::PropId::AcquisitionFrameRate, "AcquisitionFrameRate");
    PrintInteger(properties, ic4::PropId::PayloadSize, "PayloadSize", criticalReadsSucceeded, true);
    PrintInteger(properties, ic4::PropId::DeviceStreamChannelCount, "DeviceStreamChannelCount", criticalReadsSucceeded);
    PrintInteger(properties, ic4::PropId::DeviceStreamChannelPacketSize, "DeviceStreamChannelPacketSize", criticalReadsSucceeded);
    PrintInteger(properties, ic4::PropId::DeviceLinkThroughputLimit, "DeviceLinkThroughputLimit", criticalReadsSucceeded);
    PrintEnumeration(properties, ic4::PropId::DeviceLinkThroughputLimitMode, "DeviceLinkThroughputLimitMode");
    PrintEnumeration(properties, ic4::PropId::DeviceTLType, "DeviceTLType");

    bool streamSucceeded = true;
    if (probeStream) {
        streamSucceeded = ProbeStream(grabber, streamWaitMs);
    }

    if (!criticalReadsSucceeded) {
        std::cerr << "Probe result: device is enumerated and opens, but PayloadSize cannot be read.\n";
        return 2;
    }
    if (!streamSucceeded) {
        std::cerr << "Probe result: core properties are readable, but official-style stream acquisition failed.\n";
        return 4;
    }

    std::cout << "Probe result: core transport properties are readable";
    if (probeStream) std::cout << " and official-style stream acquisition succeeded";
    std::cout << ".\n";
    return 0;
}
