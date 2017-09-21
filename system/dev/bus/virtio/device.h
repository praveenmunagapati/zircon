// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#pragma once

#include <zircon/types.h>

#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/pci.h>
#include <zx/handle.h>
#include <fbl/mutex.h>
#include <threads.h>
#include <virtio/virtio.h>
#include <virtio/backends.h>

namespace virtio {

class Device {
public:
    Device(zx_device_t* bus_device);
    virtual ~Device();

    zx_device_t* bus_device() { return bus_device_; }
    zx_device_t* device() { return device_; }

    virtual zx_status_t Init() = 0;

    void StartIrqThread();

    // interrupt cases that devices may override
    virtual void IrqRingUpdate() {}
    virtual void IrqConfigChange() {}

    // used by Ring class to manipulate config registers
    void SetRing(uint16_t index, uint16_t count, zx_paddr_t pa_desc, zx_paddr_t pa_avail, zx_paddr_t pa_used);
    uint16_t GetRingSize(uint16_t index);
    void RingKick(uint16_t ring_index);

protected:
    void Reset();
    void StatusAcknowledgeDriver();
    void StatusDriverOK();

    zx::UniquePtr<VirtioBackend> backend_;
    zx::handle irq_handle_ = {};
    static int IrqThreadEntry(void* arg);
    void IrqWorker();

    zx_status_t GetFeatures(uint64_t& features);
    zx_status_t RequestFeatures(uint64_t& features);

    // members
    zx_device_t* bus_device_ = nullptr;
    fbl::Mutex lock_;

    // irq thread object
    thrd_t irq_thread_ = {};

    // DDK device
    zx_device_t* device_ = nullptr;
    zx_protocol_device_t device_ops_ = {};
};

