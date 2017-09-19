// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>

#include <zircon/compiler.h>
#include <zircon/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <virtio/virtio.h>

// implemented in virtio_driver.cpp
extern zx_status_t virtio_bind(void* ctx, zx_device_t* device, void** cookie);

static zx_driver_ops_t virtio_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = virtio_bind,
};

ZIRCON_DRIVER_BEGIN(virtio, virtio_driver_ops, "zircon", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PCI),
    // BI_ABORT_IF(NE, BIND_PCI_VID, VIRTIO_VENDOR_ID),
    // BI_MATCH_IF(EQ, BIND_PCI_DID, VIRTIO_DEV_TYPE_BLOCK),
    BI_MATCH_IF(EQ, BIND_PCI_DID, VIRTIO_DEV_TYPE_ENTROPY),
    // BI_MATCH_IF(EQ, BIND_PCI_DID, VIRTIO_DEV_TYPE_NETWORK),
    // BI_MATCH_IF(EQ, BIND_PCI_DID, VIRTIO_DEV_TYPE_T_BLOCK),
    BI_MATCH_IF(EQ, BIND_PCI_DID, VIRTIO_DEV_TYPE_T_ENTROPY),
    // BI_MATCH_IF(EQ, BIND_PCI_DID, VIRTIO_DEV_TYPE_T_NETWORK),
    // BI_MATCH_IF(EQ, BIND_PCI_DID, VIRTIO_DEV_TYPE_GPU),
    BI_ABORT(),
ZIRCON_DRIVER_END(virtio)
