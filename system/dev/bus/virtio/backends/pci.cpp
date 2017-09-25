// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/protocol/pci.h>
#include <virtio/backends/backends.h>

// For reading the virtio specific vendor capabilities that can be in PIO or MMIO space
#define cap_field(field) static_cast<uint8_t>(offsetof(virtio_pci_cap_t, field))
static void ReadVirtioCap(pci_protocol_t* pci, uint8_t offset, virtio_pci_cap& cap) {
    cap.cap_vndr = pci_config_read8(pci, offset + cap_field(cap_vndr));
    cap.cap_next = pci_config_read8(pci, offset + cap_field(cap_next));
    cap.cap_len  = pci_config_read8(pci, offset + cap_field(cap_len));
    cap.cfg_type = pci_config_read8(pci, offset + cap_field(cfg_type));
    cap.bar = pci_config_read8(pci, offset + cap_field(bar));
    cap.offset = pci_config_read32(pci, offset + cap_field(offset));
    cap.length = pci_config_read32(pci, offset + cap_field(length));
}
#undef cap_field

zx_status_t PciBackend::Bind(void) {
    fbl::AutoLock lock(&backend_lock_);
    zx_handle_t tmp_handle;

    // save off handles to things
    memcpy(&pci_, pci, sizeof(pci_protocol_t));
    info_ = info;

    // Look for an MSIX capability so we can use the knowledge for offsets later
    has_msix_ = !!(pci_get_first_capability(&pci_, kPciCapIdMsix));

    // enable bus mastering
    zx_status_t r;
    if ((r = pci_enable_bus_master(&pci_, true)) != ZX_OK) {
        VIRTIO_ERROR("cannot enable bus master %d\n", r);
        return r;
    }

    // try to set up our IRQ mode
    if (pci_set_irq_mode(&pci_, ZX_PCIE_IRQ_MODE_MSI, 1)) {
        if (pci_set_irq_mode(&pci_, ZX_PCIE_IRQ_MODE_LEGACY, 1)) {
            VIRTIO_ERROR("failed to set irq mode\n");
            return -1;
        } else {
            LTRACEF("using legacy irq mode\n");
        }
    }

    r = pci_map_interrupt(&pci_, 0, &tmp_handle);
    if (r != ZX_OK) {
        VIRTIO_ERROR("failed to map irq %d\n", r);
        return r;
    }
    irq_handle_.reset(tmp_handle);

    LTRACEF("irq handle %u\n", irq_handle_.get());

    // try to parse capabilities
    for (uint8_t off = pci_get_first_capability(&pci_, kPciCapIdVendor);
            off != 0;
            off = pci_get_next_capability(&pci_, off, kPciCapIdVendor)) {
        virtio_pci_cap_t cap;

        ReadVirtioCap(&pci_, off, cap);
        switch (cap.cfg_type) {
        case VIRTIO_PCI_CAP_COMMON_CFG:
            CommonCfgCallback(cap);
            break;
        case VIRTIO_PCI_CAP_NOTIFY_CFG:
            // 4.1.4.4 notify_off_multiplier is a 32bit field following this capability
            notify_off_mul_ = pci_config_read32(&pci_, off + sizeof(virtio_pci_cap_t));
            NotifyCfgCallback(cap);
            break;
        case VIRTIO_PCI_CAP_ISR_CFG:
            IsrCfgCallback(cap);
            break
        case VIRTIO_PCI_CAP_DEVICE_CFG:
            DeviceCfgCallback(cap);
            break;
        case VIRTIO_PCI_CAP_PCI_CFG:
            PciCfgCallback(cap);
            break;
        }
    }

    return ZX_OK;
}


// PciModernBackend definitions begin here for devices that are on the PCI bus
// and use MMIO bars for virtio operation.
void PciModernBackend::ConfigWrite(uint16_t offset, const T value) {
    auto ptr = *reinterpret_cast<volatile T*>(common_config_ + offset);
    *ptr = value;
}

// Virtio 1.0 Section 4.1.3:
// 64-bit fields are to be treated as two 32-bit fields, with low 32 bit
// part followed by the high 32 bit part.
void PciModernBackend::ConfigWrite(uint16_t offset, const uint64_t value) {
    auto words = reinterpret_cast<volatile uint32_t*>(common_config_ + offset);
    words[0] = static_cast<uint32_t(value & UINT32_MAX);
    words[1] = static_cast<uint32_t(value >> 32);
}

void PciModernBackend::ConfigRead(uint16_t offset, T* value) {
    assert(value);
    *value = *reinterpret_cast<volatile T*>(common_config_ + offset);
}

// Attempt to map a bar found in a capability structure. If it has already been
// mapped and we have stored a valid handle in the structure then just return
// ZX_OK.
zx_status_t PciModernBackend::MapBar(uint8_t bar) {
    DEBUG_ASSERT(bar < countof(bar_));
    if (bar_[bar].mmio_handle != ZX_HANDLE_INVALID) {
        return ZX_OK;
    }

    size_t size;
    zx_handle_t handle;
    void* base;
    zx_status_t s = pci_map_resource(&pci_, PCI_RESOURCE_BAR_0 + bar,
                                     ZX_CACHE_POLICY_UNCACHED_DEVICE, &base, &sz, &handle);
    if (s != ZX_OK) {
        LTRACEF("Failed to map bar %u: %d\n", bar, s);
        return s;
    }

    bar_[bar].mmio_base = base;
    bar_[bar].mmio_handle = handle;
    return ZX_OK;
}

void PciModernBackend::CommonCfgCallback(const virtio_pci_cap_t& cap) {
    TRACE_ENTRY;
    if (MapBar(cap.bar) != ZX_OK) {
        return;
    }

    common_config_ = bar_[cap.bar].mmio_base + cap.offset;
}
void PciModernBackend::NotifyCfgCallback(const virtio_pci_cap_t& cap) {
    TRACE_ENTRY;
    if (MapBar(cap.bar) != ZX_OK) {
        return;
    }

    uint16_t queue_notify_off;
    ConfigRead(VIRTIO_PCI_COMMON_CFG_QUEUE_NOTIFY_OFF, &queue_notify_off);
    notify_base_ = static_cast<volatile uint16_t*>(bar_cap.bar].mmio_base + cap.offset);

    // Virtio Spec 4.1.4.4.1 cap.length must satisfy this condition
    assert(cap.length >= queue_notify_off * notify_off_mul + 2);
}

void PciModernBackend::IsrCfgCallback(const virtio_pci_cap_t& cap) {
    TRACE_ENTRY;
    if (MapBar(cap.bar) != ZX_OK) {
        return;
    }

    isr_status_ = static_cast<volatile uint32_t*>(bar_[cap.bar].mmio_base + cap.offset);
}

void PciModernBackend::DeviceCfgCallback(const virtio_pci_cap_t& cap) {
    TRACE_ENTRY;
    if (MapBar(cap.bar) != ZX_OK) {
        return;
    }

    device_config_ = bar_[cap.bar].mmio_base + cap.offset;
}

void PciModernBackend::PciCfgCallback(const virtio_pci_cap_t& cap) {
    // We are not using this capability presently since we can make the
    // bars for direct memory access.
}
