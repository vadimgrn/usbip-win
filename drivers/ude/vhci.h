/*
 * Copyright (C) 2022 - 2024 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#pragma once

#include "context.h"

namespace usbip
{

_Function_class_(EVT_WDF_DRIVER_DEVICE_ADD)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS DeviceAdd(_In_ WDFDRIVER, _Inout_ WDFDEVICE_INIT *init);

} // namespace usbip


namespace usbip::vhci
{

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
int claim_roothub_port(_In_ UDECXUSBDEVICE device);

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
int reclaim_roothub_port(_In_ UDECXUSBDEVICE device);

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
wdf::ObjectRef get_device(_In_ WDFDEVICE vhci, _In_ int port);

enum class detach_call { async_wait, async_nowait, direct };

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void detach_all_devices(_In_ WDFDEVICE vhci, _In_ detach_call how);

struct imported_device;
enum class state;

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS fill(_Out_ imported_device &dev, _In_ const device_ctx_ext &ext, _In_ int port);

inline auto fill(_Out_ imported_device &dev, _In_ const device_ctx &ctx)
{
        return fill(dev, *ctx.ext, ctx.port);
}

} // namespace usbip::vhci
