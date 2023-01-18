/*
 * Copyright (C) 2022 - 2023 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#pragma once

#include <libdrv\codeseg.h>
#include <libdrv\ch9.h>
#include <libdrv/wdf_cpp.h>

#include <wdfusb.h>
#include <UdeCx.h>

#include <initguid.h>
#include <usbip\vhci.h>


/*
 * Macro WDF_TYPE_NAME_TO_TYPE_INFO (see WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE)
 * makes impossible to declare context type with the same name in different namespaces.
 */

namespace wsk
{
        struct SOCKET;
}

namespace usbip
{

/*
 * Context space for WDFDEVICE, Virtual Host Controller Interface.
 * Parent is WDFDRIVER.
 */
struct vhci_ctx
{
        UDECXUSBDEVICE devices[vhci::TOTAL_PORTS]; // do not access directly, functions must be used
        KSPIN_LOCK lock;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(vhci_ctx, get_vhci_ctx)

struct wsk_context;
struct device_ctx;

/*
 * Context extention for device_ctx. 
 *
 * TCP/IP connection must be established before creation of UDECXUSBDEVICE because UdecxUsbDeviceInitSetSpeed 
 * must be called prior UdecxUsbDeviceCreate. So, these data can't be stored in device_ctx. 
 * The server's response on command OP_REQ_IMPORT contains required usbip_usb_device.speed. 
 * 
 * device_ctx_ext can't be embedded into device_ctx because SocketContext must be passed to WskSocket(). 
 * Pointer to instance of device_ctx_ext will be passed.
 * 
 * Alternative is to claim portnum in vhci_ctx.devices and pass it as SocketContext.
 */
struct device_ctx_ext
{
        device_ctx *ctx;
        wsk::SOCKET *sock;

        // from vhci::ioctl_plugin
        PSTR busid;
        UNICODE_STRING node_name;
        UNICODE_STRING service_name;
        UNICODE_STRING serial; // user-defined
        //
        
        vhci::ioctl_imported_dev_data dev; // for ioctl_imported_dev
};

/*
 * Context space for UDECXUSBDEVICE -  virtual (emulated) USB device.
 */
struct device_ctx
{
        device_ctx_ext *ext; // must be free-d

        auto sock() const { return ext->sock; }
        auto speed() const { return ext->dev.speed; }
        auto devid() const { return ext->dev.devid; }

        WDFDEVICE vhci; // parent, virtual (emulated) host controller interface
        UDECXUSBENDPOINT ep0; // default control pipe
        WDFQUEUE queue; // requests that are waiting for USBIP_RET_SUBMIT from a server

        int port; // vhci_ctx.devices[port - 1]
        volatile bool unplugged;

        bool skip_select_config; // from the upper filter
        seqnum_t seqnum; // @see next_seqnum

        // for WSK receive
        WDFWORKITEM recv_hdr;
        using received_fn = NTSTATUS (wsk_context&);
        received_fn *received;
        size_t receive_size;
};        
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(device_ctx, get_device_ctx)

inline auto get_device(_In_ device_ctx *ctx)
{
        NT_ASSERT(ctx);
        return static_cast<UDECXUSBDEVICE>(WdfObjectContextGetObject(ctx));
}

WDF_DECLARE_CONTEXT_TYPE(UDECXUSBDEVICE); // WdfObjectGet_UDECXUSBDEVICE
inline auto& get_device(_In_ WDFQUEUE queue) // for device_ctx.queue
{
        return *WdfObjectGet_UDECXUSBDEVICE(queue);
}

/*
 * Context space for UDECXUSBENDPOINT.
 */
struct endpoint_ctx
{
        UDECXUSBDEVICE device; // parent
        WDFQUEUE queue; // child
        USB_ENDPOINT_DESCRIPTOR_AUDIO descriptor;
};        
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(endpoint_ctx, get_endpoint_ctx)

WDF_DECLARE_CONTEXT_TYPE(UDECXUSBENDPOINT); // WdfObjectGet_UDECXUSBENDPOINT
inline auto& get_endpoint(_In_ WDFQUEUE queue) // use get_device() for device_ctx.queue
{
        return *WdfObjectGet_UDECXUSBENDPOINT(queue);
}

enum request_status : LONG { REQ_ZERO, REQ_SEND_COMPLETE, REQ_RECV_COMPLETE, REQ_CANCELED, REQ_NO_HANDLE };

/*
 * Context space for WDFREQUEST.
 * It's OK to get a context for request that the driver doesn't own if it isn't completed.
 */
struct request_ctx
{
        seqnum_t seqnum;
        request_status status;
        UDECXUSBENDPOINT endpoint;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(request_ctx, get_request_ctx)

_IRQL_requires_max_(DISPATCH_LEVEL)
inline auto atomic_set_status(_Inout_ request_ctx &ctx, _In_ request_status status)
{
        NT_ASSERT(status != REQ_ZERO);
        NT_ASSERT(status != REQ_NO_HANDLE);
        static_assert(sizeof(ctx.status) == sizeof(LONG));
        auto oldval = InterlockedCompareExchange(reinterpret_cast<LONG*>(&ctx.status), status, REQ_ZERO);
        return static_cast<request_status>(oldval);
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
inline auto get_vhci(_In_ WDFREQUEST Request)
{
        auto queue = WdfRequestGetIoQueue(Request);
        return WdfIoQueueGetDevice(queue);
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
seqnum_t next_seqnum(_Inout_ device_ctx &dev, _In_ bool dir_in);

constexpr auto extract_num(seqnum_t seqnum) { return seqnum >> 1; }
constexpr auto extract_dir(seqnum_t seqnum) { return usbip_dir(seqnum & 1); }
constexpr bool is_valid_seqnum(seqnum_t seqnum) { return extract_num(seqnum); }

constexpr UINT32 make_devid(UINT16 busnum, UINT16 devnum)
{
        return (busnum << 16) | devnum;
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
inline void sched_receive_usbip_header(_In_ device_ctx &ctx)
{
        if (!ctx.unplugged) [[likely]]
            WdfWorkItemEnqueue(ctx.recv_hdr);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS create_device_ctx_ext(_Out_ device_ctx_ext* &d, _In_ const vhci::ioctl_plugin &r);

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void free(_In_ device_ctx_ext *ext);

} // namespace usbip
