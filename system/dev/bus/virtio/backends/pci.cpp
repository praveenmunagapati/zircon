
zx_status_t PciBackend::Bind(pci_protocol_t*, zx_pcie_device_info_t info) {
    fbl::AutoLock lock(&lock_);
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
    if (mapbar(cap.bar) != ZX_OK) {
        return;
    }

    common_config_ = bar_[cap.bar].mmio_base + cap.offset;
}
void PciModernBackend::NotifyCfgCallback(const virtio_pci_cap_t& cap) {
    TRACE_ENTRY;
    if (mapbar(cap.bar) != ZX_OK) {
        return;
    }

    notify_off_mul_ = pci_config_read32(&pci_, notify_mul_off);
    notify_base_ = static_cast<volatile uint16_t*>(bar_cap.bar].mmio_base + cap.offset);

}

void PciModernBackend::IsrCfgCallback(const virtio_pci_cap_t& cap) {
    TRACE_ENTRY;
    if (mapbar(cap.bar) != ZX_OK) {
        return;
    }

    isr_status_ = static_cast<volatile uint32_t*>(bar_[cap.bar].mmio_base + cap.offset);
}

void PciModernBackend::DeviceCfgCallback(const virtio_pci_cap_t& cap) {
    TRACE_ENTRY;
    if (mapbar(cap.bar) != ZX_OK) {
        return;
    }

    device_config_ = bar_[cap.bar].mmio_base + cap.offset;
}

void PciModernBackend::PciCfgCallback(const virtio_pci_cap_t& cap) {
    // We are not using this capability presently since we can make the
    // bars for direct memory access.
}
