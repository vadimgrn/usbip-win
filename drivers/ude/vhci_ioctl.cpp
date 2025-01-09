/*
 * Copyright (C) 2022 - 2024 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "vhci_ioctl.h"
#include "trace.h"
#include "vhci_ioctl.tmh"

#include "context.h"
#include "vhci.h"
#include "device.h"
#include "network.h"
#include "ioctl.h"
#include "persistent.h"

#include <usbip\proto_op.h>

#include <libdrv\dbgcommon.h>
#include <libdrv\strconv.h>
#include <libdrv\irp.h>

#include <ntstrsafe.h>
#include <usbuser.h>

namespace
{

using namespace usbip;

static_assert(sizeof(vhci::imported_device_location::service) == NI_MAXSERV);
static_assert(sizeof(vhci::imported_device_location::host) == NI_MAXHOST);

enum { ARG_INFO, ARG_FUNCTION, ARG_AI }; // the fourth parameter is used by WSK subsystem

struct workitem_ctx
{
        WDFDEVICE vhci;
        WDFREQUEST request;

        device_ctx_ext *ext;
        ADDRINFOEXW *addrinfo; // list head
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(workitem_ctx, get_workitem_ctx)

_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
PAGED auto set_args(_In_ WDFREQUEST request, _In_ const char *function, _In_opt_ const ADDRINFOEXW *ai = nullptr)
{
        PAGED_CODE();
        auto irp = WdfRequestWdmGetIrp(request);

        libdrv::argv<ARG_INFO>(irp) = reinterpret_cast<void*>(WdfRequestGetInformation(request)); // backup
        libdrv::argv<ARG_FUNCTION>(irp) = const_cast<char*>(function);
        libdrv::argv<ARG_AI>(irp) = const_cast<ADDRINFOEXW*>(ai);

        return irp;
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
PAGED void log(_In_ const usbip_usb_device &d)
{
        PAGED_CODE();
        TraceDbg("usbip_usb_device(path '%s', busid %s, busnum %d, devnum %d, %!usb_device_speed!,"
                "vid %#x, pid %#x, rev %#x, class/sub/proto %x/%x/%x, "
                "bConfigurationValue %d, bNumConfigurations %d, bNumInterfaces %d)", 
                d.path, d.busid, d.busnum, d.devnum, d.speed, 
                d.idVendor, d.idProduct, d.bcdDevice,
                d.bDeviceClass, d.bDeviceSubClass, d.bDeviceProtocol, 
                d.bConfigurationValue, d.bNumConfigurations, d.bNumInterfaces);
}

/*
 * @see <linux>/tools/usb/usbip/src/usbipd.c, recv_request_import
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto send_req_import(_In_ device_ctx_ext &ext)
{
        PAGED_CODE();

        struct {
                op_common hdr{ USBIP_VERSION, OP_REQ_IMPORT, ST_OK };
                op_import_request body{};
        } req;

        static_assert(sizeof(req) == sizeof(req.hdr) + sizeof(req.body)); // packed

        if (auto &busid = req.body.busid; auto err = libdrv::unicode_to_utf8(busid, sizeof(busid), ext.busid)) {
                Trace(TRACE_LEVEL_ERROR, "unicode_to_utf8('%!USTR!') %!STATUS!", &ext.busid, err);
                return err;
        }

        PACK_OP_COMMON(false, &req.hdr);
        PACK_OP_IMPORT_REQUEST(false, &req.body);

        return send(ext.sock, memory::stack, &req, sizeof(req));
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS recv_rep_import(_In_ device_ctx_ext &ext, _In_ memory pool, _Out_ op_import_reply &reply)
{
        PAGED_CODE();
        RtlZeroMemory(&reply, sizeof(reply));

        if (auto err = recv_op_common(ext.sock, OP_REP_IMPORT)) {
                return err;
        }

        if (auto err = recv(ext.sock, pool, &reply, sizeof(reply))) {
                Trace(TRACE_LEVEL_ERROR, "Receive op_import_reply %!STATUS!", err);
                return err;
        }
        PACK_OP_IMPORT_REPLY(false, &reply);

        if (char busid[sizeof(reply.udev.busid)];
            auto err = libdrv::unicode_to_utf8(busid, sizeof(busid), ext.busid)) {
                Trace(TRACE_LEVEL_ERROR, "unicode_to_utf8('%!USTR!') %!STATUS!", &ext.busid, err);
                return err;
        } else if (strncmp(reply.udev.busid, busid, sizeof(busid))) {
                Trace(TRACE_LEVEL_ERROR, "Received busid '%s' != '%s'", reply.udev.busid, busid);
                return USBIP_ERROR_PROTOCOL;
        }

        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto import_remote_device(_Inout_ device_ctx_ext &ext)
{
        PAGED_CODE();

        if (auto err = send_req_import(ext)) {
                Trace(TRACE_LEVEL_ERROR, "Send OP_REQ_IMPORT %!STATUS!", err);
                return err;
        }

        op_import_reply reply;
        if (auto err = recv_rep_import(ext, memory::stack, reply)) {
                return err;
        }
 
        auto &udev = reply.udev; 
        log(udev);

        if (auto d = &ext.dev) {
                d->devid = make_devid(static_cast<UINT16>(udev.busnum), static_cast<UINT16>(udev.devnum));
                d->speed = static_cast<usb_device_speed>(udev.speed);
                d->vendor = udev.idVendor;
                d->product = udev.idProduct;
        }

        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS plugin(_Out_ int &port, _In_ UDECXUSBDEVICE device)
{
        PAGED_CODE();

        if (port = vhci::claim_roothub_port(device); port) {
                TraceDbg("port %d claimed", port);
        } else {
                Trace(TRACE_LEVEL_ERROR, "All roothub ports are occupied");
                return USBIP_ERROR_PORTFULL;
        }

        auto &dev = *get_device_ctx(device);
        auto speed = dev.speed();

        UDECX_USB_DEVICE_PLUG_IN_OPTIONS options; 
        UDECX_USB_DEVICE_PLUG_IN_OPTIONS_INIT(&options);

        auto &portnum = speed < USB_SPEED_SUPER ? options.Usb20PortNumber : options.Usb30PortNumber;
        portnum = port;

        if (auto err = UdecxUsbDevicePlugIn(device, &options)) {
                Trace(TRACE_LEVEL_ERROR, "UdecxUsbDevicePlugIn %!STATUS!", err);
                return err;
        }

        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto start_device(_Out_ int &port, _In_ UDECXUSBDEVICE device)
{
        PAGED_CODE();

        if (auto err = plugin(port, device)) {
                return err;
        }

        return device::recv_thread_start(device);
}

/*
 * TCP_NODELAY is not supported, see WSK_FLAG_NODELAY.
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto set_options(_In_ wsk::SOCKET *sock)
{
        PAGED_CODE();

        auto keepalive = [] (auto idle, auto cnt, auto intvl) constexpr { return idle + cnt*intvl; };

        int idle = 0;
        int cnt = 0;
        int intvl = 0;

        if (auto err = get_keepalive_opts(sock, &idle, &cnt, &intvl)) {
                Trace(TRACE_LEVEL_ERROR, "get_keepalive_opts %!STATUS!", err);
                return err;
        }

        Trace(TRACE_LEVEL_VERBOSE, "get keepalive: idle(%d sec) + cnt(%d)*intvl(%d sec) => %d sec", 
                idle, cnt, intvl, keepalive(idle, cnt, intvl));

        enum { IDLE = 30, CNT = 9, INTVL = 10 };

        if (auto err = set_keepalive(sock, IDLE, CNT, INTVL)) {
                Trace(TRACE_LEVEL_ERROR, "set_keepalive %!STATUS!", err);
                return err;
        }

        bool optval{};
        if (auto err = get_keepalive(sock, optval)) {
                Trace(TRACE_LEVEL_ERROR, "get_keepalive %!STATUS!", err);
                return err;
        }

        NT_VERIFY(!get_keepalive_opts(sock, &idle, &cnt, &intvl));

        Trace(TRACE_LEVEL_VERBOSE, "set keepalive: idle(%d sec) + cnt(%d)*intvl(%d sec) => %d sec", 
                idle, cnt, intvl, keepalive(idle, cnt, intvl));

        bool ok = optval && keepalive(idle, cnt, intvl) == keepalive(IDLE, CNT, INTVL);
        return ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto connected(_In_ WDFREQUEST request, _Inout_ device_ctx_ext* &ext)
{
        PAGED_CODE();
        Trace(TRACE_LEVEL_INFORMATION, "%!USTR!:%!USTR!", &ext->node_name, &ext->service_name);

        vhci::ioctl::plugin_hardware *r{};
        NT_VERIFY(NT_SUCCESS(WdfRequestRetrieveInputBuffer(request, sizeof(*r), reinterpret_cast<PVOID*>(&r), nullptr)));

        auto vhci = get_vhci(request);

        if (auto err = import_remote_device(*ext)) {
                return err;
        }

        UDECXUSBDEVICE dev{};
        if (auto err = device::create(dev, vhci, ext)) {
                return err;
        }
        ext = nullptr; // now dev owns it

        if (auto err = start_device(r->port, dev)) {
                WdfObjectDelete(dev); // UdecxUsbDevicePlugIn failed or was not called
                return err;
        }

        Trace(TRACE_LEVEL_INFORMATION, "dev %04x plugged in, port %d", ptr04x(dev), r->port);
        return STATUS_SUCCESS;
}

/*
 * @see Using IRPs with Winsock Kernel Functions
 */
_Function_class_(IO_COMPLETION_ROUTINE)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS irp_complete(
        _In_ DEVICE_OBJECT*, _In_ IRP *irp, _In_reads_(_Inexpressible_("varies")) void *context)
{
        if (irp->PendingReturned) {
                IoMarkIrpPending(irp); // must be called
        }

        WdfWorkItemEnqueue(static_cast<WDFWORKITEM>(context));
        return StopCompletion;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto create_socket(_Inout_ wsk::SOCKET* &sock, _In_ const ADDRINFOEXW &ai)
{
        PAGED_CODE();
        NT_ASSERT(!sock);

        if (auto err = socket(sock, static_cast<ADDRESS_FAMILY>(ai.ai_family), 
                                static_cast<USHORT>(ai.ai_socktype), ai.ai_protocol, 
                                WSK_FLAG_CONNECTION_SOCKET, nullptr, nullptr)) {
                NT_ASSERT(!sock);
                Trace(TRACE_LEVEL_ERROR, "socket %!STATUS!", err);
                return err;
        }

        if (auto err = set_options(sock)) {
                return err;
        }

        SOCKADDR_INET any { // see INADDR_ANY, IN6ADDR_ANY_INIT
                .si_family = static_cast<ADDRESS_FAMILY>(ai.ai_family)
        };

        if (auto err = bind(sock, reinterpret_cast<SOCKADDR*>(&any))) {
                Trace(TRACE_LEVEL_ERROR, "bind %!STATUS!", err);
                return err;
        }

        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto connect(
        _In_ WDFREQUEST request, _In_ WDFWORKITEM wi, _Inout_ wsk::SOCKET* &sock, _In_ const ADDRINFOEXW &ai)
{
        PAGED_CODE();

        if (auto &sa = *reinterpret_cast<SOCKADDR_INET*>(ai.ai_addr); sa.si_family == AF_INET) {
                auto &v4 = sa.Ipv4;
                TraceDbg("%!IPADDR!", v4.sin_addr.s_addr);
        } else {
                auto &v6 = sa.Ipv6;
                TraceDbg("%!BIN!", WppBinary(&v6.sin6_addr, sizeof(v6.sin6_addr)));
        }

        if (auto err = create_socket(sock, ai)) {
                return err;
        }

        auto irp = set_args(request, __func__, &ai);
        IoSetCompletionRoutine(irp, irp_complete, wi, true, true, true);

        auto st = connect(sock, ai.ai_addr, irp); // completion handler will be called anyway
        TraceDbg("%!STATUS!", st);

        return STATUS_PENDING;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto on_connect(
        _In_ WDFREQUEST request, _In_ WDFWORKITEM wi, _Inout_ device_ctx_ext* &ext, _In_ const ADDRINFOEXW &ai)
{
        PAGED_CODE();

        auto st = WdfRequestGetStatus(request);

        if (NT_SUCCESS(st)) {
                st = connected(request, ext);
                NT_ASSERT(st != STATUS_PENDING);
        } else {
                NT_VERIFY(NT_SUCCESS(close(ext->sock)));
                free(ext->sock);

                if (st != STATUS_CANCELLED && ai.ai_next) {
                        st = connect(request, wi, ext->sock, *ai.ai_next);
                }
        }

        return st;
}

_Function_class_(EVT_WDF_WORKITEM)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
PAGED void NTAPI complete(_In_ WDFWORKITEM wi)
{
        PAGED_CODE();
        auto &ctx = *get_workitem_ctx(wi);

        auto request = ctx.request;
        auto irp = WdfRequestWdmGetIrp(request);

        WdfRequestSetInformation(request, libdrv::argvi<ULONG_PTR, ARG_INFO>(irp)); // restore
        auto function = libdrv::argv<const char*, ARG_FUNCTION>(irp);

        auto st = WdfRequestGetStatus(request);
        TraceDbg("%s %!STATUS!", function, st);

        if (auto ai = libdrv::argv<ADDRINFOEXW*, ARG_AI>(irp)) {
                st = on_connect(request, wi, ctx.ext, *ai);
        } else if (NT_SUCCESS(st)) { // on_addrinfo
                NT_ASSERT(ctx.addrinfo);
                st = connect(request, wi, ctx.ext->sock, *ctx.addrinfo);
        }

        if (st != STATUS_PENDING) {
                TraceDbg("req %04x, %!STATUS!", ptr04x(request), st);
                WdfRequestComplete(request, st);
                WdfObjectDelete(wi); // do not use ctx.request more, see workitem_cleanup
        }
}

/*
 * ctx.request could be already completed.
 */
_Function_class_(EVT_WDF_OBJECT_CONTEXT_CLEANUP)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void workitem_cleanup(_In_ WDFOBJECT obj)
{
        PAGED_CODE();
        
        auto wi = static_cast<WDFWORKITEM>(obj); // WdfWorkItemGetParentObject(wi) returns NULL
        auto &ctx = *get_workitem_ctx(wi); 

        TraceDbg("request %04x, addrinfo %04x, device_ctx_ext %04x", 
                  ptr04x(ctx.request), ptr04x(ctx.addrinfo), ptr04x(ctx.ext));

        wsk::free(ctx.addrinfo);
        ctx.addrinfo = nullptr;

        if (auto &ext = ctx.ext) {
                close_socket(ext->sock);
                free(ext);
                ext = nullptr;
        }
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto create_workitem(_Out_ WDFWORKITEM &wi, _In_ WDFOBJECT parent)
{
        PAGED_CODE();

        WDF_WORKITEM_CONFIG cfg;
        WDF_WORKITEM_CONFIG_INIT(&cfg, complete);
        cfg.AutomaticSerialization = false;

        WDF_OBJECT_ATTRIBUTES attr; // WdfSynchronizationScopeNone is inherited from the driver object
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, workitem_ctx);

        attr.EvtCleanupCallback = workitem_cleanup;
        attr.ParentObject = parent;

        return WdfWorkItemCreate(&cfg, &attr, &wi);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void getaddrinfo(_In_ WDFREQUEST request, _In_ WDFWORKITEM wi, _Inout_ workitem_ctx &ctx)
{
        PAGED_CODE();
        auto &ext = *ctx.ext;

        ADDRINFOEXW hints {
                .ai_flags = AI_NUMERICSERV,
                .ai_family = AF_UNSPEC,
                .ai_socktype = SOCK_STREAM,
                .ai_protocol = IPPROTO_TCP // zero isn't work
        };

        auto irp = set_args(request, __func__);
        IoSetCompletionRoutine(irp, irp_complete, wi, true, true, true);
                         
        NT_ASSERT(!ctx.addrinfo);
        auto st = wsk::getaddrinfo(ctx.addrinfo, &ext.node_name, &ext.service_name, &hints, irp);
        TraceDbg("%!STATUS!", st);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto plugin_hardware( _In_ WDFREQUEST request, _In_ const vhci::ioctl::plugin_hardware &r)
{
        PAGED_CODE();

        Trace(TRACE_LEVEL_INFORMATION, "%s:%s, busid %s", r.host, r.service, r.busid);
        auto vhci = get_vhci(request);

        WDFWORKITEM wi{};
        if (auto err = create_workitem(wi, vhci)) {
                Trace(TRACE_LEVEL_ERROR, "WdfWorkItemCreate %!STATUS!", err);
                return err;
        }
        auto &ctx = *get_workitem_ctx(wi);

        ctx.vhci = vhci;
        ctx.request = request;

        if (auto err = create_device_ctx_ext(ctx.ext, r)) {
                WdfObjectDelete(wi);
                return err;
        }

        getaddrinfo(request, wi, ctx); // completion handler will be called anyway
        return STATUS_PENDING;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS plugin_hardware(_In_ WDFREQUEST request)
{
        PAGED_CODE();
        WdfRequestSetInformation(request, 0);

        vhci::ioctl::plugin_hardware *r{};

        if (size_t length{}; 
            auto err = WdfRequestRetrieveInputBuffer(request, sizeof(*r), reinterpret_cast<PVOID*>(&r), &length)) {
                return err;
        } else if (length != sizeof(*r)) {
                return STATUS_INVALID_BUFFER_SIZE;
        } else if (r->size != sizeof(*r)) {
                Trace(TRACE_LEVEL_ERROR, "plugin_hardware.size %lu != sizeof(plugin_hardware) %Iu", 
                                          r->size, sizeof(*r));

                return USBIP_ERROR_ABI;
        }

        r->port = 0;

        constexpr auto written = offsetof(vhci::ioctl::plugin_hardware, port) + sizeof(r->port);
        WdfRequestSetInformation(request, written);

        return plugin_hardware(request, *r);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS plugout_hardware(_In_ WDFREQUEST request)
{
        PAGED_CODE();

        vhci::ioctl::plugout_hardware *r{};

        if (size_t length; 
            auto err = WdfRequestRetrieveInputBuffer(request, sizeof(*r), reinterpret_cast<PVOID*>(&r), &length)) {
                return err;
        } else if (length != sizeof(*r)) {
                return STATUS_INVALID_BUFFER_SIZE;
        } else if (r->size != sizeof(*r)) {
                Trace(TRACE_LEVEL_ERROR, "plugout_hardware.size %lu != sizeof(plugout_hardware) %Iu",
                                          r->size, sizeof(*r));

                return USBIP_ERROR_ABI;
        }

        TraceDbg("port %d", r->port);
        auto st = STATUS_SUCCESS;

        if (auto vhci = get_vhci(request); r->port <= 0) {
                detach_all_devices(vhci, vhci::detach_call::async_wait); // detach_call::direct can't be used here
        } else if (!is_valid_port(r->port)) {
                st = STATUS_INVALID_PARAMETER;
        } else if (auto dev = vhci::get_device(vhci, r->port)) {
                st = device::async_detach_and_wait(dev.get<UDECXUSBDEVICE>());
        } else {
                st = STATUS_DEVICE_NOT_CONNECTED;
        }

        return st;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS get_imported_devices(_In_ WDFREQUEST request)
{
        PAGED_CODE();
        WdfRequestSetInformation(request, 0);

        size_t outlen;
        vhci::ioctl::get_imported_devices *r;

        if (auto err = WdfRequestRetrieveOutputBuffer(request, sizeof(*r), reinterpret_cast<PVOID*>(&r), &outlen)) {
                return err;
        } else if (r->size != sizeof(*r)) {
                Trace(TRACE_LEVEL_ERROR, "get_imported_devices.size %lu != sizeof(get_imported_devices) %Iu", 
                                          r->size, sizeof(*r));

                return USBIP_ERROR_ABI;
        }

        auto devices_size = outlen - offsetof(vhci::ioctl::get_imported_devices, devices); // size of array

        auto max_cnt = devices_size/sizeof(*r->devices);
        NT_ASSERT(max_cnt);

        auto vhci = get_vhci(request);
        ULONG cnt = 0;

        for (int port = 1; port <= ARRAYSIZE(vhci_ctx::devices); ++port) {
                if (auto dev = vhci::get_device(vhci, port); !dev) {
                        //
                } else if (cnt == max_cnt) {
                        return STATUS_BUFFER_TOO_SMALL;
                } else if (auto ctx = get_device_ctx(dev.get()); auto err = fill(r->devices[cnt++], *ctx)) {
                        return err;
                }
        }

        TraceDbg("%lu device(s) reported", cnt);

        auto written = vhci::ioctl::get_imported_devices_size(cnt);
        NT_ASSERT(written <= outlen);
        WdfRequestSetInformation(request, written);

        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto set_persistent(_In_ WDFREQUEST request)
{
        PAGED_CODE();

        void *buf{};
        size_t length{};
        if (auto err = WdfRequestRetrieveInputBuffer(request, 0, &buf, &length)) {
                return err;
        }

        Registry key;
        if (auto err = open_parameters_key(key, KEY_SET_VALUE)) {
                return err;
        }

        UNICODE_STRING val_name;
        RtlUnicodeStringInit(&val_name, persistent_devices_value_name);

        auto st = WdfRegistryAssignValue(key.get(), &val_name, REG_MULTI_SZ, ULONG(length), buf);
        if (st) {
                Trace(TRACE_LEVEL_ERROR, "WdfRegistryAssignValue(%!USTR!) %!STATUS!, length %Iu", &val_name, st, length);
        }
        return st;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto get_persistent(_In_ WDFREQUEST request)
{
        PAGED_CODE();
        WdfRequestSetInformation(request, 0);

        void *buf{};
        size_t length{};
        if (auto err = WdfRequestRetrieveOutputBuffer(request, 0, &buf, &length)) {
                return err;
        }

        Registry key;
        if (auto err = open_parameters_key(key, KEY_QUERY_VALUE)) {
                return err;
        }

        UNICODE_STRING val_name;
        RtlUnicodeStringInit(&val_name, persistent_devices_value_name);

        ULONG actual{};
        auto type = REG_NONE;
        auto st = WdfRegistryQueryValue(key.get(), &val_name, ULONG(length), buf, &actual, &type);

        if (st) {
                Trace(TRACE_LEVEL_ERROR, "WdfRegistryQueryValue(%!USTR!) %!STATUS!, length %Iu", &val_name, st, length);
        } else if (type != REG_MULTI_SZ) {
                Trace(TRACE_LEVEL_ERROR, "WdfRegistryQueryValue(%!USTR!): type(%ul) != REG_MULTI_SZ(%ul)", &val_name, type, REG_MULTI_SZ);
                st = STATUS_INVALID_CONFIG_VALUE;
                actual = 0;
        }

        WdfRequestSetInformation(request, actual);
        return st;
}

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

        TraceDbg("%s(%#08lX), OutputBufferLength %Iu, InputBufferLength %Iu", 
                  device_control_name(IoControlCode), IoControlCode, OutputBufferLength, InputBufferLength);

        NTSTATUS st;

        switch (IoControlCode) {
        case vhci::ioctl::PLUGIN_HARDWARE:
                st = plugin_hardware(Request);
                break;
        case vhci::ioctl::PLUGOUT_HARDWARE:
                st = plugout_hardware(Request);
                break;
        case IOCTL_USB_USER_REQUEST:
                NT_ASSERT(!has_urb(Request));
                if (USBUSER_REQUEST_HEADER *hdr; 
                    NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, sizeof(*hdr), 
                                                             reinterpret_cast<PVOID*>(&hdr), nullptr))) {
                        TraceDbg("USB_USER_REQUEST -> %s(%#08lX)", usbuser_request_name(hdr->UsbUserRequest), 
                                  hdr->UsbUserRequest);
                }
                [[fallthrough]];
        default:
                auto vhci = WdfIoQueueGetDevice(Queue);

                st = UdecxWdfDeviceTryHandleUserIoctl(vhci, Request) ? // PASSIVE_LEVEL
                        STATUS_PENDING : STATUS_INVALID_DEVICE_REQUEST;
        }

        if (st != STATUS_PENDING) {
                TraceDbg("%!STATUS!, Information %Ix", st, WdfRequestGetInformation(Request));
                WdfRequestComplete(Request, st);
        }
}

/*
 * There is an internal lock on the registry, but that’s just to ensure that registry operations are atomic; 
 * that is, that if one thread writes a value to the registry and another thread reads that same value 
 * from the registry, then the value that comes back is either the value before the write took place 
 * or the value after the write took place, but not some sort of mixture of the two.
 *
 * @see Raymond Chen, The inability to lock someone out of the registry is a feature, not a bug
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED decltype(get_persistent) *get_parallel_handler(_In_ ULONG IoControlCode)
{
        PAGED_CODE();

        switch (IoControlCode) {
        case vhci::ioctl::GET_IMPORTED_DEVICES:
                return get_imported_devices;
        case vhci::ioctl::SET_PERSISTENT:
                return set_persistent;
        case vhci::ioctl::GET_PERSISTENT:
                return get_persistent;
        default:
                return nullptr;
        }
}

_Function_class_(EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
PAGED void device_control_parallel(
        _In_ WDFQUEUE Queue,
        _In_ WDFREQUEST Request,
        _In_ size_t OutputBufferLength,
        _In_ size_t InputBufferLength,
        _In_ ULONG IoControlCode)
{
        PAGED_CODE();

        NTSTATUS st;

        if (auto handler = get_parallel_handler(IoControlCode)) {
                TraceDbg("%s(%#08lX), OutputBufferLength %Iu, InputBufferLength %Iu", 
                          device_control_name(IoControlCode), IoControlCode, OutputBufferLength, InputBufferLength);

                st = handler(Request);
        } else {
                auto vhci = WdfIoQueueGetDevice(Queue);
                auto def_queue = WdfDeviceGetDefaultQueue(vhci);

                st = WdfRequestForwardToIoQueue(Request, def_queue); // -> device_control

                if (NT_SUCCESS(st)) [[likely]] {
                        st = STATUS_PENDING;
                } else {
                        Trace(TRACE_LEVEL_ERROR, "WdfRequestForwardToIoQueue %!STATUS!", st);
                }
        }

        if (st != STATUS_PENDING) {
                TraceDbg("%!STATUS!, Information %Ix", st, WdfRequestGetInformation(Request));
                WdfRequestComplete(Request, st);
        }
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

        WDF_IO_QUEUE_CONFIG_INIT(&cfg, WdfIoQueueDispatchParallel);
        cfg.PowerManaged = PowerManaged;
        cfg.EvtIoDeviceControl = device_control_parallel;

        if (WDFQUEUE queue{}; auto err = WdfIoQueueCreate(vhci, &cfg, &attr, &queue)) {
                Trace(TRACE_LEVEL_ERROR, "WdfIoQueueCreate %!STATUS!", err);
                return err;
        } else if (err = WdfDeviceConfigureRequestDispatching(vhci, queue, WdfRequestTypeDeviceControl); err) {
                Trace(TRACE_LEVEL_ERROR, "WdfDeviceConfigureRequestDispatching %!STATUS!", err);
                return err;
        }

        return STATUS_SUCCESS;
}
