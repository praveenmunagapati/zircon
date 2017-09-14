// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device.h"

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <hw/inout.h>
#include <zircon/status.h>
#include <fbl/auto_lock.h>
#include <pretty/hexdump.h>

#include "trace.h"

#define LOCAL_TRACE 0

namespace virtio {

Device::Device(zx_device_t* bus_device)
    : bus_device_(bus_device) {
    LTRACE_ENTRY;
    device_ops_.version = DEVICE_OPS_VERSION;
}

Device::~Device() {
    LTRACE_ENTRY;
}

// For reading the virtio specific vendor capabilities that can be in PIO or MMIO space
static void ReadVirtioCap(pci_protocol_t* pci, uint8_t offset, virtio_pci_cap& cap) {
    cap.cap_vndr     = pci_config_read8(pci, static_cast<uint8_t>(offset + offsetof(virtio_pci_cap, cap_vndr)));
    cap.cap_next     = pci_config_read8(pci, static_cast<uint8_t>(offset + offsetof(virtio_pci_cap, cap_next)));
    cap.cap_len      = pci_config_read8(pci, static_cast<uint8_t>(offset + offsetof(virtio_pci_cap, cap_len)));
    cap.cfg_type     = pci_config_read8(pci, static_cast<uint8_t>(offset + offsetof(virtio_pci_cap, cfg_type)));
    cap.bar          = pci_config_read8(pci, static_cast<uint8_t>(offset + offsetof(virtio_pci_cap, bar)));
    cap.padding[0]   = pci_config_read8(pci, static_cast<uint8_t>(offset + offsetof(virtio_pci_cap, padding)));
    cap.padding[1]   = pci_config_read8(pci, static_cast<uint8_t>(offset + offsetof(virtio_pci_cap, padding) + 1));
    cap.padding[2]   = pci_config_read8(pci, static_cast<uint8_t>(offset + offsetof(virtio_pci_cap, padding) + 2));
    cap.offset      = pci_config_read32(pci, static_cast<uint8_t>(offset + offsetof(virtio_pci_cap, offset)));
    cap.length      = pci_config_read32(pci, static_cast<uint8_t>(offset + offsetof(virtio_pci_cap, length)));
}

zx_status_t Device::MapBar(uint8_t i) {
    if (bar_[i].mmio_handle != ZX_HANDLE_INVALID)
        return ZX_OK;

    uint64_t sz;
    zx_handle_t tmp_handle;

    zx_status_t r = pci_map_resource(&pci_, PCI_RESOURCE_BAR_0 + i, ZX_CACHE_POLICY_UNCACHED_DEVICE,
                                     (void**)&bar_[i].mmio_base, &sz, &tmp_handle);
    if (r != ZX_OK) {
        VIRTIO_ERROR("cannot map io %d\n", bar_[i].mmio_handle.get());
        return r;
    }
    bar_[i].mmio_handle.reset(tmp_handle);
    LTRACEF("bar %hhu mmio_base %p, sz %#" PRIx64 "\n", i, bar_[i].mmio_base, sz);

    return ZX_OK;
}

zx_status_t Device::GetFeatures(uint64_t& features) {
    return ZX_OK;
}

zx_status_t Device::RequestFeatures(uint64_t& features) {
    return ZX_OK;
}

zx_status_t Device::Bind(pci_protocol_t* pci, zx_pcie_device_info_t info) {
    LTRACE_ENTRY;

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
        virtio_pci_cap cap;

        ReadVirtioCap(&pci_, off, cap);
        LTRACEF("cap type %#hhx next %#hhx len %#hhx cfg_type %#hhx bar %#hhx "
                "offset %#x length %#x\n", cap.cap_vndr, cap.cap_next, cap.cap_len, cap.cfg_type, cap.bar, cap.offset, cap.length);
        switch (cap.cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG: {
                MapBar(cap.bar);
                mmio_regs_.common_config = (volatile virtio_pci_common_cfg*)((uintptr_t)bar_[cap.bar].mmio_base + cap.offset);
                LTRACEF("common_config %p\n", mmio_regs_.common_config);
                break;
            }
            case VIRTIO_PCI_CAP_NOTIFY_CFG: {
                MapBar(cap.bar);
                mmio_regs_.notify_base = (volatile uint16_t*)((uintptr_t)bar_[cap.bar].mmio_base + cap.offset);
                uint8_t notify_mul_off = static_cast<uint8_t>(off + offsetof(virtio_pci_notify_cap, notify_off_multiplier));
                mmio_regs_.notify_mul = pci_config_read32(&pci_, notify_mul_off);
                LTRACEF("notify_base %p\n", mmio_regs_.notify_base);
                LTRACEF("notify_mul %x\n", mmio_regs_.notify_mul);
                break;
            }
            case VIRTIO_PCI_CAP_ISR_CFG: {
                MapBar(cap.bar);
                mmio_regs_.isr_status = (volatile uint32_t*)((uintptr_t)bar_[cap.bar].mmio_base + cap.offset);
                LTRACEF("isr_status %p\n", mmio_regs_.isr_status);
                break;
            }
            case VIRTIO_PCI_CAP_DEVICE_CFG: {
                MapBar(cap.bar);
                mmio_regs_.device_config = (volatile void*)((uintptr_t)bar_[cap.bar].mmio_base + cap.offset);
                LTRACEF("device_config %p\n", mmio_regs_.device_config);
                break;
            }
        }
    }

    // if we've found mmio pointers to everything from the capability structure,
    // then skip mapping bar0, since we don't need legacy pio access from BAR0
    if (!(mmio_regs_.common_config && mmio_regs_.notify_base &&
                mmio_regs_.isr_status && mmio_regs_.device_config)) {
        zx_pci_resource_t bar0;
        r = MapBar(0);
        if (r == ZX_OK) {
            LTRACEF("bar_[0].mmio_base %p\n", bar_[0].mmio_base);
        } else if (r == ZX_ERR_WRONG_TYPE) {
            r = pci_get_resource(&pci_, PCI_RESOURCE_BAR_0, &bar0);
            if (r != ZX_OK || bar0.type != PCI_RESOURCE_TYPE_PIO) {
                VIRTIO_ERROR("failed to get PIO BAR0: %d\n", r);
                return -1;
            }
            bar0_pio_base_ = static_cast<uint32_t>(bar0.pio_addr);
            LTRACEF("Using PIO bar0, base: %d\n", bar0_pio_base_);
        } else {
            TRACEF("Unhandled device layout!");
            return ZX_ERR_BAD_STATE;
        }
    }

    LTRACE_EXIT;

    return ZX_OK;
}

void Device::Unbind() {
    device_remove(device_);
}

void Device::Release() {
    irq_handle_.reset();
}

void Device::IrqWorker() {
    LTRACEF("started\n");
    zx_status_t rc;
    assert(irq_handle_);

    while (irq_handle_) {
        if ((rc = zx_interrupt_wait(irq_handle_.get())) < 0) {
            printf("virtio: error while waiting for interrupt: %s\n",
                zx_status_get_string(rc));
            continue;
        }

        uint32_t irq_status;
        if (mmio_regs_.isr_status) {
            irq_status = *mmio_regs_.isr_status;
        } else {
            irq_status = inp((bar0_pio_base_ + VIRTIO_PCI_ISR_STATUS) & 0xffff);
        }

        LTRACEF_LEVEL(2, "irq_status %#x\n", irq_status);

        if ((rc = zx_interrupt_complete(irq_handle_.get())) < 0) {
            printf("virtio: error while completing interrupt: %s\n",
                zx_status_get_string(rc));
            continue;
        }

        if (irq_status == 0)
            continue;

        // grab the mutex for the duration of the irq handlers
        fbl::AutoLock lock(&lock_);

        if (irq_status & 0x1) { /* used ring update */
            IrqRingUpdate();
        }
        if (irq_status & 0x2) { /* config change */
            IrqConfigChange();
        }
    }
}

int Device::IrqThreadEntry(void* arg) {
    Device* d = static_cast<Device*>(arg);

    d->IrqWorker();

    return 0;
}

void Device::StartIrqThread() {
    thrd_create_with_name(&irq_thread_, IrqThreadEntry, this, "virtio-irq-thread");
    thrd_detach(irq_thread_);
}

namespace {
template <typename T> T ioread(uint16_t port);

template<> uint8_t ioread<uint8_t>(uint16_t port) { return inp(port); }
template<> uint16_t ioread<uint16_t>(uint16_t port) { return inpw(port); }
template<> uint32_t ioread<uint32_t>(uint16_t port) { return inpd(port); }

template <typename T> void iowrite(uint16_t port, T val);

template<> void iowrite<uint8_t>(uint16_t port, uint8_t val) { return outp(port, val); }
template<> void iowrite<uint16_t>(uint16_t port, uint16_t val) { return outpw(port, val); }
template<> void iowrite<uint32_t>(uint16_t port, uint32_t val) { return outpd(port, val); }

template <typename T>
void mmiowrite(volatile void* ptr, T val);

template<>
void mmiowrite<uint32_t>(volatile void* ptr, uint32_t val) {
    *reinterpret_cast<volatile uint32_t*>(ptr) = val;
}

template<>
void mmiowrite<uint64_t>(volatile void* ptr, uint64_t val) {
    // Virtio 1.0 Section 4.1.3:
    // 64-bit fields are to be treated as two 32-bit fields, with low 32 bit
    // part followed by the high 32 bit part.
    auto words = reinterpret_cast<volatile uint32_t*>(ptr);
    mmiowrite(&words[0], static_cast<uint32_t>(val & UINT32_MAX));
    mmiowrite(&words[1], static_cast<uint32_t>(val >> 32));
}

} // anon namespace

template <typename T>
T Device::ReadConfigBar(uint16_t offset) {
    if (bar0_pio_base_) {
        uint16_t port = (bar0_pio_base_ + offset) & 0xffff;
        LTRACEF_LEVEL(3, "port %#x\n", port);
        return ioread<T>(port);
    } else if (bar_[0].mmio_base) {
        volatile T *addr = (volatile T *)((uintptr_t)bar_[0].mmio_base + offset);
        LTRACEF_LEVEL(3, "addr %p\n", addr);
        return *addr;
    } else {
        // XXX implement
        assert(0);
        return 0;
    }
}

template <typename T>
void Device::WriteConfigBar(uint16_t offset, T val) {
    if (bar0_pio_base_) {
        uint16_t port = (bar0_pio_base_ + offset) & 0xffff;
        LTRACEF_LEVEL(3, "port %#x\n", port);
        iowrite<T>(port, val);
    } else if (bar_[0].mmio_base) {
        volatile T *addr = (volatile T *)((uintptr_t)bar_[0].mmio_base + offset);
        LTRACEF_LEVEL(3, "addr %p\n", addr);
        *addr = val;
    } else {
        // XXX implement
        assert(0);
    }
}

zx_status_t Device::CopyDeviceConfig(void* _buf, size_t len) {
    if (mmio_regs_.device_config) {
        auto buf = static_cast<uint8_t*>(_buf);
        auto config = static_cast<volatile uint8_t*>(mmio_regs_.device_config);
        for (size_t i = 0; i < len; i++) {
            buf[i] = config[i];
        }
    } else {
        // XXX handle MSI vs noMSI
        size_t offset = (has_msix_) ? VIRTIO_PCI_CONFIG_OFFSET_MSIX : VIRTIO_PCI_CONFIG_OFFSET_NOMSIX;

        uint8_t* buf = (uint8_t*)_buf;
        for (size_t i = 0; i < len; i++) {
            buf[i] = ReadConfigBar<uint8_t>((offset + i) & 0xffff);
        }
    }

    return ZX_OK;
}

uint16_t Device::GetRingSize(uint16_t index) {
    if (!mmio_regs_.common_config) {
        if (bar0_pio_base_) {
            return inpw((bar0_pio_base_ + VIRTIO_PCI_QUEUE_SIZE) & 0xffff);
        } else if (bar_[0].mmio_base) {
            volatile uint16_t *ptr16 = (volatile uint16_t *)((uintptr_t)bar_[0].mmio_base + VIRTIO_PCI_QUEUE_SIZE);
            return *ptr16;
        } else {
            // XXX implement
            assert(0);
            return 0;
        }
    } else {
        mmio_regs_.common_config->queue_select = index;
        return mmio_regs_.common_config->queue_size;
    }
}

void Device::SetRing(uint16_t index, uint16_t count, zx_paddr_t pa_desc, zx_paddr_t pa_avail, zx_paddr_t pa_used) {
    LTRACEF("index %u, count %u, pa_desc %#" PRIxPTR ", pa_avail %#" PRIxPTR ", pa_used %#" PRIxPTR "\n",
            index, count, pa_desc, pa_avail, pa_used);

    if (!mmio_regs_.common_config) {
        if (bar0_pio_base_) {
            outpw((bar0_pio_base_ + VIRTIO_PCI_QUEUE_SELECT) & 0xffff, index);
            outpw((bar0_pio_base_ + VIRTIO_PCI_QUEUE_SIZE) & 0xffff, count);
            outpd((bar0_pio_base_ + VIRTIO_PCI_QUEUE_PFN) & 0xffff, (uint32_t)(pa_desc / PAGE_SIZE));
        } else if (bar_[0].mmio_base) {
            volatile uint16_t *ptr16 = (volatile uint16_t *)((uintptr_t)bar_[0].mmio_base + VIRTIO_PCI_QUEUE_SELECT);
            *ptr16 = index;
            ptr16 = (volatile uint16_t *)((uintptr_t)bar_[0].mmio_base + VIRTIO_PCI_QUEUE_SIZE);
            *ptr16 = count;
            volatile uint32_t *ptr32 = (volatile uint32_t *)((uintptr_t)bar_[0].mmio_base + VIRTIO_PCI_QUEUE_PFN);
            *ptr32 = (uint32_t)(pa_desc / PAGE_SIZE);
        } else {
            // XXX implement
            assert(0);
        }
    } else {
        mmio_regs_.common_config->queue_select = index;
        mmio_regs_.common_config->queue_size = count;
        mmiowrite(&mmio_regs_.common_config->queue_desc, pa_desc);
        mmiowrite(&mmio_regs_.common_config->queue_avail, pa_avail);
        mmiowrite(&mmio_regs_.common_config->queue_used, pa_used);
        mmio_regs_.common_config->queue_enable = 1;
    }
}

void Device::RingKick(uint16_t ring_index) {
    LTRACEF("index %u\n", ring_index);
    if (!mmio_regs_.notify_base) {
        if (bar0_pio_base_) {
            outpw((bar0_pio_base_ + VIRTIO_PCI_QUEUE_NOTIFY) & 0xffff, ring_index);
        } else {
            // XXX implement
            assert(0);
        }
    } else {
        volatile uint16_t* notify = mmio_regs_.notify_base + ring_index * mmio_regs_.notify_mul / sizeof(uint16_t);
        LTRACEF_LEVEL(2, "notify address %p\n", notify);
        *notify = ring_index;
    }
}

void Device::Reset() {
    if (!mmio_regs_.common_config) {
        WriteConfigBar<uint8_t>(VIRTIO_PCI_DEVICE_STATUS, 0);
    } else {
        mmio_regs_.common_config->device_status = 0;
    }
}

void Device::StatusAcknowledgeDriver() {
    if (!mmio_regs_.common_config) {
        uint8_t val = ReadConfigBar<uint8_t>(VIRTIO_PCI_DEVICE_STATUS);
        val |= VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
        WriteConfigBar(VIRTIO_PCI_DEVICE_STATUS, val);
    } else {
        mmio_regs_.common_config->device_status |= VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    }
}

void Device::StatusDriverOK() {
    if (!mmio_regs_.common_config) {
        uint8_t val = ReadConfigBar<uint8_t>(VIRTIO_PCI_DEVICE_STATUS);
        val |= VIRTIO_STATUS_DRIVER_OK;
        WriteConfigBar(VIRTIO_PCI_DEVICE_STATUS, val);
    } else {
        mmio_regs_.common_config->device_status |= VIRTIO_STATUS_DRIVER_OK;
    }
}

} // namespace virtio
