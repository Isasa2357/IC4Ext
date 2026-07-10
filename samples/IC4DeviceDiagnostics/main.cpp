#include <ic4/ic4.h>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

const char* ArgValue(int argc, char** argv, const char* name)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == name) return argv[i + 1];
    }
    return nullptr;
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

    ic4::Error err;
    const auto displayName = interface.interfaceDisplayName(err);
    std::cout << "  interface   : " << (err.isError() ? std::string("<error: ") + ErrorText(err) + ">" : displayName) << "\n";

    err = {};
    const auto tlName = interface.transportLayerName(err);
    std::cout << "  tlName      : " << (err.isError() ? std::string("<error: ") + ErrorText(err) + ">" : tlName) << "\n";

    err = {};
    const auto tlVersion = interface.transportLayerVersion(err);
    std::cout << "  tlVersion   : " << (err.isError() ? std::string("<error: ") + ErrorText(err) + ">" : tlVersion) << "\n";

    err = {};
    const auto tlType = interface.transportLayerType(err);
    std::cout << "  tlType      : " << (err.isError() ? std::string("<error: ") + ErrorText(err) + ">" : TransportTypeName(tlType)) << "\n";
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

} // namespace

int main(int argc, char** argv)
{
    ic4::InitLibraryConfig initConfig;
    initConfig.defaultErrorHandlerBehavior = ic4::ErrorHandlerBehavior::Ignore;
    if (!ic4::initLibrary(initConfig)) {
        std::cerr << "ic4::initLibrary failed\n";
        return 1;
    }

    int resultCode = 0;
    {
        ic4::Error enumError;
        const auto devices = ic4::DeviceEnum::enumDevices(enumError);
        if (enumError.isError()) {
            std::cerr << "Device enumeration failed: " << ErrorText(enumError) << "\n";
            ic4::exitLibrary();
            return 1;
        }

        std::cout << "IC4 devices found: " << devices.size() << "\n";
        for (std::size_t i = 0; i < devices.size(); ++i) {
            PrintDevice(i, devices[i]);
        }

        const char* indexText = ArgValue(argc, argv, "--device-index");
        const char* serialText = ArgValue(argc, argv, "--serial");
        if (!indexText && !serialText) {
            std::cout << "\nSpecify --device-index N or --serial SERIAL to open and probe one device.\n";
            ic4::exitLibrary();
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
                ic4::exitLibrary();
                return 1;
            }
        } else {
            const int index = std::atoi(indexText);
            if (index < 0 || static_cast<std::size_t>(index) >= devices.size()) {
                std::cerr << "--device-index is out of range\n";
                ic4::exitLibrary();
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
            ic4::exitLibrary();
            return 1;
        }

        if (!grabber.deviceOpen(devices[*selectedIndex], grabberError)) {
            std::cerr << "deviceOpen failed: " << ErrorText(grabberError) << "\n";
            ic4::exitLibrary();
            return 1;
        }
        std::cout << "deviceOpen: success\n";

        auto properties = grabber.devicePropertyMap(grabberError);
        if (grabberError.isError() || !properties) {
            std::cerr << "devicePropertyMap failed: " << ErrorText(grabberError) << "\n";
            grabber.deviceClose(grabberError);
            ic4::exitLibrary();
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

        if (!criticalReadsSucceeded) {
            std::cerr << "Probe result: device is enumerated and opens, but PayloadSize cannot be read.\n";
            resultCode = 2;
        } else {
            std::cout << "Probe result: core transport properties are readable.\n";
        }

        ic4::Error closeError;
        grabber.deviceClose(closeError);
        if (closeError.isError()) {
            std::cerr << "deviceClose warning: " << ErrorText(closeError) << "\n";
        }
    }

    ic4::exitLibrary();
    return resultCode;
}
