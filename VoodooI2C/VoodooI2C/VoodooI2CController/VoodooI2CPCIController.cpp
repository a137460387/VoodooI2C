//
//  VoodooI2CPCIController.cpp
//  VoodooI2C
//
//  Created by Alexandre on 02/08/2017.
//  Copyright © 2017 Alexandre Daoud. All rights reserved.
//

#include "VoodooI2CPCIController.hpp"

#define super VoodooI2CController
OSDefineMetaClassAndStructors(VoodooI2CPCIController, VoodooI2CController);

void VoodooI2CPCIController::configurePCI() {
    IOLog("%s::%s Set PCI power state D0\n", getName(), physical_device.name);
    auto pci_device = physical_device.pci_device;
    pci_device->enablePCIPowerManagement(kPCIPMCSPowerStateD0);

    pci_device->setBusMasterEnable(true);
    pci_device->setMemoryEnable(true);
}

IOReturn VoodooI2CPCIController::getACPIDevice() {
    // Get ACPI device path
    OSObject* acpi_path_object = physical_device.pci_device->copyProperty(kACPIDevicePathKey);
    OSString* acpi_path = OSDynamicCast(OSString, acpi_path_object);
    if (!acpi_path) {
        OSSafeReleaseNULL(acpi_path_object);
        return kIOReturnError;
    }

    // Get ACPI device
    IORegistryEntry* entry = IORegistryEntry::fromPath(acpi_path->getCStringNoCopy());
    acpi_path->release();

    physical_device.acpi_device = OSDynamicCast(IOACPIPlatformDevice, entry);
    if (!physical_device.acpi_device) {
        OSSafeReleaseNULL(entry);
        return kIOReturnError;
    }

    return kIOReturnSuccess;
}

IOReturn VoodooI2CPCIController::setPowerState(unsigned long whichState, IOService* whatDevice) {
    if (whatDevice != this)
        return kIOPMAckImplied;

    if (whichState == 0) {  // index of kIOPMPowerOff state in VoodooI2CIOPMPowerStates
        if (physical_device.awake) {
            physical_device.awake = false;
            unmapMemory();
            IOLog("%s::%s Going to sleep\n", getName(), physical_device.name);
        }
    } else {
        if (!physical_device.awake) {
            configurePCI();
            if (mapMemory() != kIOReturnSuccess)
                IOLog("%s::%s Could not map memory\n", getName(), physical_device.name);
            skylakeLPSSResetHack();

            physical_device.awake = true;
            IOLog("%s::%s Woke up\n", getName(), physical_device.name);
        }
    }
    return kIOPMAckImplied;
}

void VoodooI2CPCIController::skylakeLPSSResetHack() {
    writeRegister((LPSS_PRIV_RESETS_FUNC | LPSS_PRIV_RESETS_IDMA), (LPSS_PRIV + LPSS_PRIV_RESETS));
}

bool VoodooI2CPCIController::start(IOService* provider) {
    if (!super::start(provider)) {
        return false;
    }

    physical_device.pci_device = OSDynamicCast(IOPCIDevice, provider);

    if (getACPIDevice() != kIOReturnSuccess) {
        IOLog("%s::%s Could not get ACPI device for PCI provider\n", getName(), physical_device.name);
        return false;
    }

    configurePCI();

    // Huawei MateBook: DSDT _PS0 is empty and enablePCIPowerManagement(D0)
    // has no effect, leaving the controller in D3hot with unreadable MMIO.
    // Force D0 by clearing the PMCSR power-state bits directly.
    UInt32 pmcsr = physical_device.pci_device->configRead32(0x84);
    if ((pmcsr & 0x3) != 0) {
        physical_device.pci_device->configWrite32(0x84, pmcsr & ~0x3u);  // 低2位清0 = D0,其余位保留
        IOSleep(10);  // D3hot→D0 需要时间稳定
    }

    if (mapMemory() != kIOReturnSuccess) {
        IOLog("%s::%s Could not map memory\n", getName(), physical_device.name);
        return false;
    }

    skylakeLPSSResetHack();

    if (publishNub() != kIOReturnSuccess) {
        IOLog("%s::%s Could not publish nub\n", getName(), physical_device.name);
        return false;
    }

    registerService();

    return true;
}

void VoodooI2CPCIController::stop(IOService* provider) {
    OSSafeReleaseNULL(physical_device.acpi_device);
    super::stop(provider);
}
