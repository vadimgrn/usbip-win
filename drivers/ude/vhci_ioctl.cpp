/*
 * Copyright (C) 2022 - 2024 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "vhci_ioctl.h"
#include "trace.h"
#include "vhci_ioctl.tmh"

#include "context.h"
#include "vhci.h"

#include <usbip\proto_op.h>

#include <libdrv\dbgcommon.h>
#include <libdrv\strconv.h>
#include <libdrv\irp.h>

#include <ntstrsafe.h>
#include <usbuser.h>

namespace
{

using namespace usbip;

/*
 * IRP_MJ_DEVICE_CONTROL
 * 
 * This is a public driver API. How to maintain its compatibility for libusbip users.
 * 1.IOCTLs are like syscals on Linux. Once IOCTL code is released, its input/output data remain 
 *   the same for lifetime.
 * 2.If this is not possible, new IOCTL code must be added.
 * 3.IOCTL could be removed (unlike syscals) for various reasons. This will break backward compatibility.
 *   It can be declared as deprecated in some release and removed afterwards. 
 *   The removed IOCTL code must never be reused.
 */
_Function_class_(EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
PAGED void device_control(
        _In_ WDFQUEUE Queue,
        _In_ WDFREQUEST Request,
        _In_ size_t OutputBufferLength,
        _In_ size_t InputBufferLength,
        _In_ ULONG IoControlCode)
{
        PAGED_CODE();

        TraceDbg("%#08lX, OutputBufferLength %Iu, InputBufferLength %Iu", 
                  IoControlCode, OutputBufferLength, InputBufferLength);

        NTSTATUS st = STATUS_NOT_IMPLEMENTED;

        switch (IoControlCode) {
        case vhci::ioctl::PLUGIN_HARDWARE:
        case vhci::ioctl::PLUGOUT_HARDWARE:
        case vhci::ioctl::GET_IMPORTED_DEVICES:
        case vhci::ioctl::SET_PERSISTENT:
        case vhci::ioctl::GET_PERSISTENT:
                break;
        default:
                if (auto vhci = WdfIoQueueGetDevice(Queue);
                    UdecxWdfDeviceTryHandleUserIoctl(vhci, Request)) { // PASSIVE_LEVEL
                        return;
                } else {
                        st = STATUS_INVALID_DEVICE_REQUEST;
                }
        }

        WdfRequestComplete(Request, st);
}

} // namespace


_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::vhci::create_queues(_In_ WDFDEVICE vhci)
{
        PAGED_CODE();
        const auto PowerManaged = WdfFalse;

        WDF_OBJECT_ATTRIBUTES attr;
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ExecutionLevel = WdfExecutionLevelPassive;
        attr.ParentObject = vhci;

        WDF_IO_QUEUE_CONFIG cfg;
        WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&cfg, WdfIoQueueDispatchSequential); // WdfDeviceGetDefaultQueue
        cfg.PowerManaged = PowerManaged;
        cfg.EvtIoDeviceControl = device_control;

        if (auto err = WdfIoQueueCreate(vhci, &cfg, &attr, nullptr)) {
                Trace(TRACE_LEVEL_ERROR, "WdfIoQueueCreate %!STATUS!", err);
                return err;
        }

        return STATUS_SUCCESS;
}
