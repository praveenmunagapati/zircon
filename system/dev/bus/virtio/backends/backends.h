// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#pragma once

// Each backend will implement their own method for initialization / binding
// based on their own internal functionality. Since we will have different
// base drivers at a bind level to bind to pci vs mmio, that layer will
// be able to determine what needs to be called and what parameters to pass.
//
// ex: A device bound as a pci device will know to create a PCI backend
// with the protocol and device info parameters.
class VirtioBackend {
public:
    virtual void Init();

    // Helper method to do a complete device config copy for use in
    // IrqConfigChance.
    virtual zx_status_t CopyDeviceConfig(void* _buf, size_t len);
    // Read/Write the common config
    template <typename T> virtual void ConfigRead(uint16_t offset, T* value);
    template <typename T> virtual void ConfigWrite(uint16_t offset, const T& value);
    // Read/Write the device config
    template <typename T> virtual void DeviceConfigRead(uint16_T offset, T* value);
    template <typename T> virtual void DeviceConfigWrite(uint16_t offset, const T& value);

private:
    // This lock protects backend accesses
    fbl::Mutex lock_;
    zx_handle_t irq_handle_;
};

class PciBackend : public VirtioBackend {
public:
    zx_status_t Bind(pci_protocol_t*, zx_pcie_device_info_t info);
    // Callbacks for setting up configuration capabilities that are found
    // during Init().
    virtual void CommonCfgCallback(const virtio_pci_cap_t& cap);
    virtual void NotifyCfgCallback(const virtio_pci_cap_t& cap);
    virtual void IsrCfgCallback(const virtio_pci_cap_t& cap);
    virtual void DeviceCfgCallback(const virtio_pci_cap_t& cap);
    virtual void PciCfgCallback(const virtio_pci_cap_t& cap);

    // Copies len bytes starting at device_config_ into _buf
    // The multiplier for queue_notify_off to find the Queue Notify address
    // within a BAR. This field won't change and can be cached by Init()
    uint32_t notify_off_mul(void) const { return notify_off_mul_; }
    // Read and return the value of the isr status field. This will clear
    // the status bits for future reads.
    virtual uint8_t isr_status(void);
protected:
    uint32_t notify_off_mul_;
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
    } bar_[6] = { 0, XZ_HANDLE_INVALID };

    uint32_t* notify_base_;
    uint32_t* isr_status_;
    uintptr_t common_config_;
    uintptr_t device_config_;


} // namespace virtio
