// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/pci.h>

#include <fbl/alloc_checker.h>
#include <fbl/unique_ptr.h>

#include <zircon/compiler.h>
#include <zircon/types.h>

#include "block.h"
#include "device.h"
#include "rng.h"
#include "ethernet.h"
#include "gpu.h"
#include "trace.h"

#define LOCAL_TRACE 0

// implement driver object:

extern "C" zx_status_t virtio_bind(void* ctx, zx_device_t* device, void** cookie) {
    LTRACEF("device %p\n", device);
    zx_status_t status;
    pci_protocol_t pci;

    /* grab the pci device and configuration */
    if (device_get_protocol(device, ZX_PROTOCOL_PCI, &pci)) {
        TRACEF("no pci protocol\n");
        return -1;
    }

    zx_pcie_device_info_t info;
    status = pci_get_device_info(&pci, &info);
    if (status != ZX_OK) {
        TRACEF("failed to grab config handle\n");
        return status;
    }

    LTRACEF("pci %p\n", &pci);
    LTRACEF("0x%x:0x%x\n", info.vendor_id, info.device_id);

    fbl::unique_ptr<virtio::Device> vd = nullptr;
    switch (info.device_id) {
    case VIRTIO_DEV_TYPE_NETWORK:
    case VIRTIO_DEV_TYPE_T_NETWORK:
        LTRACEF("found net device\n");
        vd.reset(new virtio::EthernetDevice(device));
        break;
    case VIRTIO_DEV_TYPE_BLOCK:
    case VIRTIO_DEV_TYPE_T_BLOCK:
        LTRACEF("found block device\n");
        vd.reset(new virtio::BlockDevice(device));
        break;
    case VIRTIO_DEV_TYPE_GPU:
        LTRACEF("found gpu device\n");
        vd.reset(new virtio::GpuDevice(device));
        break;
    case VIRTIO_DEV_TYPE_ENTROPY:
    case VIRTIO_DEV_TYPE_T_ENTROPY:
        LTRACEF("found entropy device\n");
        vd.reset(new virtio::RngDevice(device));
        break;
    default:
        TRACEF("unhandled device id, how did this happen?\n");
        return -1;
    }

    LTRACEF("calling Bind on driver\n");
    status = vd->Bind(&pci, info);
    if (status != ZX_OK)
        return status;

    status = vd->Init();
    if (status != ZX_OK)
        return status;

    // if we're here, we're successful so drop the unique ptr ref to the object and let it live on
    vd.release();

    LTRACE_EXIT;

    return ZX_OK;
}
