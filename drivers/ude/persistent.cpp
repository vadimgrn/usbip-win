/*
 * Copyright (C) 2023 - 2024 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "persistent.h"
#include "trace.h"
#include "persistent.tmh"

#include "context.h"

#include <libdrv\strconv.h>
#include <libdrv\wait_timeout.h>
#include <resources/messages.h>

#include <ntstrsafe.h>

namespace 
{

using namespace usbip;

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto get_persistent_devices(_In_ WDFKEY key)
{
        PAGED_CODE();
        ObjectDelete col;
        
        if (WDFCOLLECTION h{};
            auto err = WdfCollectionCreate(WDF_NO_OBJECT_ATTRIBUTES, &h)) {
                Trace(TRACE_LEVEL_ERROR, "WdfCollectionCreate %!STATUS!", err);
                return col;
        } else {
                col.reset(h);
        }

        WDF_OBJECT_ATTRIBUTES str_attr;
        WDF_OBJECT_ATTRIBUTES_INIT(&str_attr);
        str_attr.ParentObject = col.get();

        UNICODE_STRING value_name;
        RtlUnicodeStringInit(&value_name, persistent_devices_value_name);

        if (auto err = WdfRegistryQueryMultiString(key, &value_name, &str_attr, col.get<WDFCOLLECTION>())) {
                Trace(TRACE_LEVEL_ERROR, "WdfRegistryQueryMultiString('%!USTR!') %!STATUS!", &value_name, err);
                col.reset();
        }

        return col;
}

constexpr auto empty(_In_ const UNICODE_STRING &s)
{
        return libdrv::empty(s) || !*s.Buffer;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto parse_string(_Out_ vhci::ioctl::plugin_hardware &r, _In_ const UNICODE_STRING &str)
{
        PAGED_CODE();

        UNICODE_STRING host;
        UNICODE_STRING service;
        UNICODE_STRING busid;

        const auto sep = L',';

        libdrv::split(host, busid, str, sep);
        if (empty(host)) {
                return STATUS_INVALID_PARAMETER;
        }

        libdrv::split(service, busid, busid, sep);
        if (empty(service) || empty(busid)) {
                return STATUS_INVALID_PARAMETER;
        }

        return copy(r.host, sizeof(r.host), host, 
                    r.service, sizeof(r.service), service, 
                    r.busid, sizeof(r.busid), busid);
}

/*
 * Target is self. 
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto make_target(_In_ WDFDEVICE vhci)
{
        PAGED_CODE();
        ObjectDelete target;

        if (WDFIOTARGET t; auto err = WdfIoTargetCreate(vhci, WDF_NO_OBJECT_ATTRIBUTES, &t)) {
                Trace(TRACE_LEVEL_ERROR, "WdfIoTargetCreate %!STATUS!", err);
                return target;
        } else {
                target.reset(t);
        }

        auto fdo = WdfDeviceWdmGetDeviceObject(vhci);

        WDF_IO_TARGET_OPEN_PARAMS params;
        WDF_IO_TARGET_OPEN_PARAMS_INIT_EXISTING_DEVICE(&params, fdo);

        if (auto err = WdfIoTargetOpen(target.get<WDFIOTARGET>(), &params)) {
                Trace(TRACE_LEVEL_ERROR, "WdfIoTargetOpen %!STATUS!", err);
                target.reset();
        }

        return target;
}

constexpr auto get_delay(_In_ ULONG attempt, _In_ ULONG cnt)
{
        NT_ASSERT(cnt);
        enum { UNIT = 10, MAX_DELAY = 30*60 }; // seconds
        return attempt > 1 ? min(UNIT*attempt/cnt, MAX_DELAY) : 0; // first two attempts without a delay
}

/*
 * WskGetAddressInfo() can return STATUS_INTERNAL_ERROR(0xC00000E5), but after some delay it will succeed.
 * This can happen after reboot if dnscache(?) service is not ready yet.
 */
constexpr auto can_retry(_In_ NTSTATUS status)
{
        switch (as_usbip_status(status)) {
        case USBIP_ERROR_VERSION:
        case USBIP_ERROR_PROTOCOL:
        case USBIP_ERROR_ABI:
        // @see op_status_t
        case USBIP_ERROR_ST_NA: 
        case USBIP_ERROR_ST_DEV_BUSY:
        case USBIP_ERROR_ST_DEV_ERR:
        case USBIP_ERROR_ST_NODEV:
        case USBIP_ERROR_ST_ERROR:
                return false;
        default:
                return true;
        }
}

/*
 * @return true - do not try to connect to this device again
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto plugin_hardware(
        _In_ const UNICODE_STRING &line, 
        _In_ WDFIOTARGET target,
        _Inout_ vhci::ioctl::plugin_hardware &req,
        _Inout_ WDF_MEMORY_DESCRIPTOR &input,
        _Inout_ WDF_MEMORY_DESCRIPTOR &output,
        [[maybe_unused]] _In_ const ULONG outlen)
{
        PAGED_CODE();

        if (auto err = parse_string(req, line)) {
                Trace(TRACE_LEVEL_ERROR, "'%!USTR!' parse %!STATUS!", &line, err);
                return true; // remove malformed string
        }

        Trace(TRACE_LEVEL_INFORMATION, "%s:%s/%s", req.host, req.service, req.busid);
        req.port = 0;

        if (ULONG_PTR BytesReturned; // send IOCTL to itself
            auto err = WdfIoTargetSendIoctlSynchronously(target, WDF_NO_HANDLE, vhci::ioctl::PLUGIN_HARDWARE, 
                                                         &input, &output, nullptr, &BytesReturned)) {
                Trace(TRACE_LEVEL_ERROR, "WdfIoTargetSendIoctlSynchronously %!STATUS!", err);
                return !can_retry(err);
        } else {
                NT_ASSERT(BytesReturned == outlen);
                return true;
        }
}

} // namespace 


_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::copy(
        _Inout_ char *host, _In_ USHORT host_sz, _In_ const UNICODE_STRING &uhost,
        _Inout_ char *service, _In_ USHORT service_sz, _In_ const UNICODE_STRING &uservice,
        _Inout_ char *busid, _In_ USHORT busid_sz, _In_ const UNICODE_STRING &ubusid)
{
        PAGED_CODE();

        struct {
                char *dst;
                USHORT dst_sz;
                const UNICODE_STRING &src;
        } const v[] = {
                {host, host_sz, uhost},
                {service, service_sz, uservice},
                {busid, busid_sz, ubusid},
        };

        for (auto &[dst, dst_sz, src]: v) {
                if (auto err = libdrv::unicode_to_utf8(dst, dst_sz, src)) {
                        Trace(TRACE_LEVEL_ERROR, "unicode_to_utf8('%!USTR!') %!STATUS!", &src, err);
                        return err;
                }
        }

        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::open_parameters_key(_Out_ Registry &key, _In_ ACCESS_MASK DesiredAccess)
{
        PAGED_CODE();

        WDFKEY k{}; 
        auto st = WdfDriverOpenParametersRegistryKey(WdfGetDriver(), DesiredAccess, WDF_NO_OBJECT_ATTRIBUTES, &k);
        if (st) {
                Trace(TRACE_LEVEL_ERROR, "WdfDriverOpenParametersRegistryKey(DesiredAccess=%lu) %!STATUS!", 
                                          DesiredAccess, st);
        }

        key.reset(k);
        return st;
}
