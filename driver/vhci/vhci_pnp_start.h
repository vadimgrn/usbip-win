#pragma once

#include "basetype.h"
#include "vhci_dev.h"

PAGEABLE NTSTATUS pnp_start_device(vdev_t *vdev, IRP *irp);
