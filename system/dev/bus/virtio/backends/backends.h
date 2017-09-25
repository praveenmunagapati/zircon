// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#pragma once

#include <ddk/protocol/pci.h>
#include <fbl/unique_ptr.h>

// Each backend will implement their own method for initialization / binding
// based on their own internal functionality. Since we will have different
// base drivers at a bind level to bind to pci vs mmio, that layer will
// be able to determine what needs to be called and what parameters to pass.
//
// ex: A device bound as a pci device will know to create a PCI backend
// with the protocol and device info parameters.
namespace virtio {

class Backend {
public:
    virtual ~Backend();

    // Read/Write the common config
    virtual void ConfigRead(uint16_t offset, uint8_t* value);
    virtual void ConfigRead(uint16_t offset, uint16_t* value);
    virtual void ConfigRead(uint16_t offset, uint32_t* value);
    virtual void ConfigRead(uint16_t offset, uint64_t* value);
    virtual void ConfigWrite(uint16_t offset, const uint8_t* value);
    virtual void ConfigWrite(uint16_t offset, const uint16_t* value);
    virtual void ConfigWrite(uint16_t offset, const uint32_t* value);
    virtual void ConfigWrite(uint16_t offset, const uint64_t* value);

    // Read/Write the device config
    virtual void DeviceConfigRead(uint16_t offset, uint8_t* value);
    virtual void DeviceConfigRead(uint16_t offset, uint16_t* value);
    virtual void DeviceConfigRead(uint16_t offset, uint32_t* value);
    virtual void DeviceConfigRead(uint16_t offset, uint64_t* value);
    virtual void DeviceConfigWrite(uint16_t offset, const uint8_t value);
    virtual void DeviceConfigWrite(uint16_t offset, const uint16_t value);
    virtual void DeviceConfigWrite(uint16_t offset, const uint32_t value);
    virtual void DeviceConfigWrite(uint16_t offset, const uint64_t value);
    // Expected to read the interrupt status out of the config based on the offset/address
    // specified by the isr capability.
    virtual uint8_t isr_status(void) = 0;

protected:
    // XXX: Should these probably should be part of the PCI backend.
    // The multiplier for queue_notify_off to find the Queue Notify address
    // within a BAR. This field won't change and can be cached by Init()
    uint32_t notify_off_mul(void) const { return notify_off_mul_; }
    // Read and return the value of the isr status field. This will clear
    // the status bits for future reads.
    uint32_t notify_off_mul_;

private:
    // This lock protects backend accesses
    fbl::Mutex backend_lock_;
    zx_handle_t irq_handle_;
};

// class MmioBackend : public Backend {
// }

class PciBackend : public Backend {
public:
    PciBackend(pci_protocol_t* pci, zx_pcie_device_info_t info)
        : pci_(*pci), info_(info) {}

    zx_status_t Bind();
    // Callbacks for setting up configuration capabilities that are found
    // during Bind().
    virtual void CommonCfgCallback(const virtio_pci_cap_t& cap);
    virtual void NotifyCfgCallback(const virtio_pci_cap_t& cap);
    virtual void IsrCfgCallback(const virtio_pci_cap_t& cap);
    virtual void DeviceCfgCallback(const virtio_pci_cap_t& cap);
    virtual void PciCfgCallback(const virtio_pci_cap_t& cap);

protected:
    // These fields will contain either an offset into IO space or
    // an address for MMIO space depending on backend.
    pci_protocol_t pci_ = { nullptr, nullptr };
    zx_pcie_device_info_t info_;
    bool has_msix_ = false;
};

class PciModernBackend : public PciBackend {
private:
    zx_status_t MapBar(uint8_t bar);

    struct bar {
        volatile void* mmio_base;
        zx::handle mmio_handle;
    } bar_[6] = { nullptr, {} };

    // XXX: If these were all made uintptr_ts then they could become protected members of
    // PciBackend and then the methods on the Modern/Legacy calls could cast them as needed.
    uint32_t* notify_base_;
    uint32_t* isr_status_;
    uintptr_t common_config_;
    uintptr_t device_config_;
};

} // namespace virtio
