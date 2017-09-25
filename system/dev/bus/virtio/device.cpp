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

#define LOCAL_TRACE 1

namespace virtio {

Device::Device(zx_device_t* bus_device, ::fbl::unique_ptr<Backend> backend)
    : backend_(backend), bus_device_(bus_device) {
    LTRACE_ENTRY;
    device_ops_.version = DEVICE_OPS_VERSION;
}

Device::~Device() {
    LTRACE_ENTRY;
}

void Device::Release() {
    irq_handle_.reset();
}

void Device::IrqWorker() {
    LTRACEF("started\n");
    zx_status_t rc;
    assert(irq_handle_);

    while (irq_handle_) {
        if ((rc = zx_interrupt_wait(irq_handle_.get())) != ZX_OK) {
            printf("virtio: error while waiting for interrupt: %s\n",
                zx_status_get_string(rc));
            continue;
        }


        // Read the status before completing the interrupt in case
        // another interrupt fires and changes the status.
        uint32_t irq_status;
        irq_status = isr_status();

        LTRACEF_LEVEL(2, "irq_status %#x\n", irq_status);

        if ((rc = zx_interrupt_complete(irq_handle_.get())) != ZX_OK) {
            printf("virtio: error while completing interrupt: %s\n",
                zx_status_get_string(rc));
            continue;
        }

        // Since we handle both interrupt types here it's possible for a
        // spurious interrupt if they come in sequence and we check isr_status
        // after both have been triggered.
        if (irq_status == 0)
            continue;

        // XXX: Unnecessary? grab the mutex for the duration of the irq handlers
        fbl::AutoLock lock(&lock_);

        if (irq_status & VIRTIO_ISR_QUEUE_INT) { /* used ring update */
            IrqRingUpdate();
        }
        if (irq_status & VIRTIO_ISR_DEV_CFG_INT) { /* config change */
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

zx_status_t Device::CopyDeviceConfig(void* _buf, size_t len) {
    assert(_buf);

    for (size_t i = 0; i < len; i++) {
        backend_->DeviceConfigRead(i, (uint8_t*)(buf + i));
    }
    // } else {
        // // XXX handle MSI vs noMSI
        // size_t offset = (has_msix_) ? VIRTIO_PCI_CONFIG_OFFSET_MSIX : VIRTIO_PCI_CONFIG_OFFSET_NOMSIX;

        // uint8_t* buf = (uint8_t*)_buf;
        // for (size_t i = 0; i < len; i++) {
            // buf[i] = ReadConfigBar<uint8_t>((offset + i) & 0xffff);
        // }
    // }

    return ZX_OK;
}

// Get the Ring size for the particular device / backend.
// This has to be proxied to a backend method because we can't
// simply do config reads to determine the information. Modern
// devices have queue selects to worry about, whereas legacy do
// not.
uint16_t Device::GetRingSize(uint16_t index) {
    return backend_->GetRingSize(index);
    // if (!mmio_regs_.common_config) {
        // if (bar0_pio_base_) {
            // return inpw((bar0_pio_base_ + VIRTIO_PCI_QUEUE_SIZE) & 0xffff);
        // } else if (bar_[0].mmio_base) {
            // volatile uint16_t *ptr16 = (volatile uint16_t *)((uintptr_t)bar_[0].mmio_base + VIRTIO_PCI_QUEUE_SIZE);
            // return *ptr16;
        // } else {
            // // XXX implement
            // assert(0);
            // return 0;
        // }
    // } else {
        // mmio_regs_.common_config->queue_select = index;
        // return mmio_regs_.common_config->queue_size;
    // }
}

// Set up ring descriptors with the backend.
void Device::SetRing(uint16_t index, uint16_t count, zx_paddr_t pa_desc, zx_paddr_t pa_avail, zx_paddr_t pa_used) {
    LTRACEF("index %u, count %u, pa_desc %#" PRIxPTR ", pa_avail %#" PRIxPTR ", pa_used %#" PRIxPTR "\n",
            index, count, pa_desc, pa_avail, pa_used);
    backend_->ConfigWrite(VIRTIO_PCI_COMMON_CFG_QUEUE_SELECT, index);
    backend_->ConfigWrite(VIRTIO_PCI_COMMON_CFG_QUEUE_SIZE, count);
    backend_->ConfigWrite(VIRTIO_PCI_COMMON_CFG_QUEUE_DESC, pa_desc);
    backend_->ConfigWrite(VIRTIO_PCI_COMMON_CFG_QUEUE_AVAIL, pa_avail);
    backend_->ConfigWrite(VIRTIO_PCI_COMMON_CFG_QUEUE_USED, pa_used);
    backend_->ConfigWrite(VIRTIO_PCI_COMMON_CFG_QUEUE_ENABLE, 1);


    // if (!mmio_regs_.common_config) {
        // if (bar0_pio_base_) {
            // outpw((bar0_pio_base_ + VIRTIO_PCI_QUEUE_SELECT) & 0xffff, index);
            // outpw((bar0_pio_base_ + VIRTIO_PCI_QUEUE_SIZE) & 0xffff, count);
            // outpd((bar0_pio_base_ + VIRTIO_PCI_QUEUE_PFN) & 0xffff, (uint32_t)(pa_desc / PAGE_SIZE));
        // } else if (bar_[0].mmio_base) {
            // volatile uint16_t *ptr16 = (volatile uint16_t *)((uintptr_t)bar_[0].mmio_base + VIRTIO_PCI_QUEUE_SELECT);
            // *ptr16 = index;
            // ptr16 = (volatile uint16_t *)((uintptr_t)bar_[0].mmio_base + VIRTIO_PCI_QUEUE_SIZE);
            // *ptr16 = count;
            // volatile uint32_t *ptr32 = (volatile uint32_t *)((uintptr_t)bar_[0].mmio_base + VIRTIO_PCI_QUEUE_PFN);
            // *ptr32 = (uint32_t)(pa_desc / PAGE_SIZE);
        // } else {
            // // XXX implement
            // assert(0);
        // }
    // } else {
        // mmio_regs_.common_config->queue_select = index;
        // mmio_regs_.common_config->queue_size = count;
        // mmiowrite(&mmio_regs_.common_config->queue_desc, pa_desc);
        // mmiowrite(&mmio_regs_.common_config->queue_avail, pa_avail);
        // mmiowrite(&mmio_regs_.common_config->queue_used, pa_used);
        // mmio_regs_.common_config->queue_enable = 1;
    // }
}

// Another method that has to be proxied to the backend due to differences
// in how Legacy vs Modern systems are laid out.
void Device::RingKick(uint16_t ring_index) {
    backend_->RingKick(ring_index);
    // LTRACEF("index %u\n", ring_index);
    // if (!mmio_regs_.notify_base) {
        // if (bar0_pio_base_) {
            // outpw((bar0_pio_base_ + VIRTIO_PCI_QUEUE_NOTIFY) & 0xffff, ring_index);
        // } else {
            // // XXX implement
            // assert(0);
        // }
    // } else {
        // volatile uint16_t* notify = mmio_regs_.notify_base + ring_index * mmio_regs_.notify_mul / sizeof(uint16_t);
        // LTRACEF_LEVEL(2, "notify address %p\n", notify);
        // *notify = ring_index;
    // }
}

void Device::Reset() {
    backend_->ConfigWrite(kCommonCfgDeviceStatus, 0);
    // if (!mmio_regs_.common_config) {V
        // WriteConfigBar<uint8_t>(VIRTIO_PCI_DEVICE_STATUS, 0);
    // } else {
        // mmio_regs_.common_config->device_status = 0;
    // }
}

void Device::StatusAcknowledgeDriver() {
    uint8_t val;
    backend_->ConfigRead(kCommonCfgDeviceStatus, &val);
    val |= VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    backend_->ConfigWrite(kCommonCfgDeviceStatus, val);
    // if (!mmio_regs_.common_config) {
        // uint8_t val = ReadConfigBar<uint8_t>(VIRTIO_PCI_DEVICE_STATUS);
        // val |= VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
        // WriteConfigBar(VIRTIO_PCI_DEVICE_STATUS, val);
    // } else {
        // mmio_regs_.common_config->device_status |= VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    // }
}

void Device::StatusDriverOK() {
    uint8_t val;
    backend_->ConfigRead(kCommonCfgDeviceStatus, &val);
    backend_->ConfigWrite(kCommonCfgDeviceStatus, val | VIRTIO_STATUS_DRIVER_OK);
    // if (!mmio_regs_.common_config) {
        // uint8_t val = ReadConfigBar<uint8_t>(VIRTIO_PCI_DEVICE_STATUS);
        // val |= VIRTIO_STATUS_DRIVER_OK;
        // WriteConfigBar(VIRTIO_PCI_DEVICE_STATUS, val);
    // } else {
        // mmio_regs_.common_config->device_status |= VIRTIO_STATUS_DRIVER_OK;
    // }
}

} // namespace virtio
