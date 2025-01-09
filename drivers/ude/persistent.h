/*
 * Copyright (C) 2023 - 2024 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#pragma once

#include <libdrv\codeseg.h>
#include <libdrv\wdf_cpp.h>

namespace usbip
{

struct vhci_ctx;
struct device_ctx;

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS open_parameters_key(_Out_ Registry &key, _In_ ACCESS_MASK DesiredAccess);

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS copy(
        _Inout_ char *host, _In_ USHORT host_sz, _In_ const UNICODE_STRING &uhost,
        _Inout_ char *service, _In_ USHORT service_sz, _In_ const UNICODE_STRING &uservice,
        _Inout_ char *busid, _In_ USHORT busid_sz, _In_ const UNICODE_STRING &ubusid);

} // namespace usbip
