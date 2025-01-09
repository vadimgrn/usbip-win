/*
 * Copyright (C) 2022 - 2024 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "device_ioctl.h"
#include "trace.h"
#include "device_ioctl.tmh"

#include "context.h"
#include "wsk_context.h"
#include "request_list.h"
#include "wsk_receive.h"
#include "proto.h"
#include "network.h"
#include "ioctl.h"

#include "filter_request.h"
#include <ude_filter\request.h>

#include <libdrv\irp.h>
#include <libdrv\pdu.h>
#include <libdrv\ch9.h>
#include <libdrv\ch11.h>
#include <libdrv\usbdsc.h>
#include <libdrv\wsk_cpp.h>
#include <libdrv\usb_util.h>
#include <libdrv\dbgcommon.h>
#include <libdrv\usbd_helper.h>

namespace
{

using namespace usbip;

/*
 * wsk_irp->Tail.Overlay.DriverContext[] are zeroed.
 *
 * The completion handler for WskReceive is executed by a high priority thread
 * and is usually called before this handler.
 * @see wsk_receive.cpp, ret_command 
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS send_complete(_In_ DEVICE_OBJECT*, _In_ IRP *wsk_irp, _In_reads_opt_(_Inexpressible_("varies")) void *context)
{
        wsk_context_ptr ctx(static_cast<wsk_context*>(context), true);

        auto request = ctx->request; // can be WDF_NO_HANDLE or already completed
        auto &dev = *ctx->dev;

        auto &wsk = wsk_irp->IoStatus;
        TraceWSK("req %04x -> wsk irp %04x, %!STATUS!, Information %Iu", 
                  ptr04x(request), ptr04x(wsk_irp), wsk.Status, wsk.Information);

        if (!request) {
                // nothing to do
        } else if (NT_SUCCESS(wsk.Status)) {
                ++dev.sent_requests;
                if (auto seqnum = ctx.seqnum(true); auto err = device::mark_request_cancelable(dev, seqnum)) {
                        auto device = get_handle(&dev);
                        device::send_cmd_unlink_and_complete(device, request, err);
                }
        } else if (device::remove_request(dev, request, false)) {
                complete(request, wsk.Status);
        } else {
                TraceDbg("req %04x not found, could not complete", ptr04x(request));
        }

        if (wsk.Status == STATUS_FILE_FORCED_CLOSED && !dev.unplugged) {
                auto device = get_handle(&dev);
                TraceDbg("dev %04x, unplugging after %!STATUS!", ptr04x(device), wsk.Status);
                device::async_detach_nowait(device);
        }

        return StopCompletion;
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
auto prepare_wsk_buf(_Inout_ WSK_BUF &buf, _Inout_ wsk_context &ctx, _Inout_opt_ const URB *transfer_buffer)
{
        NT_ASSERT(!ctx.mdl_buf);

        if (transfer_buffer && is_transfer_dir_out(ctx.hdr)) { // TransferFlags can have wrong direction
                if (auto err = make_transfer_buffer_mdl(ctx.mdl_buf, URB_BUF_LEN, IoReadAccess, *transfer_buffer)) {
                        Trace(TRACE_LEVEL_ERROR, "make_transfer_buffer_mdl %!STATUS!", err);
                        return err;
                }
        }

        ctx.mdl_hdr.next(ctx.mdl_buf); // always replace tie from previous call

        if (ctx.is_isoc) {
                NT_ASSERT(ctx.mdl_isoc);
                byteswap(ctx.isoc, number_of_packets(ctx));
                auto t = tail(ctx.mdl_hdr); // ctx.mdl_buf can be a chain
                t->Next = ctx.mdl_isoc.get();
        }

        buf.Mdl = ctx.mdl_hdr.get();
        buf.Offset = 0;
        buf.Length = get_total_size(ctx.hdr);

        NT_ASSERT(verify(buf, ctx.is_isoc));
        return STATUS_SUCCESS;
}

/*
 * switch (wdf::Lock lck(...); auto st = send(...))
 * is not used due to unspecified evaluation order of init-statement and condition.
 * switch (init-statement; condition) {} can be treated as:
 * {
 *      auto c = condition;
 *      init-statement;
 *      switch (c) {}
 * }
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
auto send(_In_opt_ UDECXUSBENDPOINT endpoint, _In_ wsk_context_ptr &ctx, _Inout_ device_ctx &dev,
        _In_ bool log_setup, _Inout_opt_ const URB* transfer_buffer = nullptr)
{
        auto request = ctx->request; // can be WDF_NO_HANDLE, do not access after send

        WSK_BUF buf{};
        if (auto err = prepare_wsk_buf(buf, *ctx, transfer_buffer)) {
                return err;
        } else {
                char str[DBG_USBIP_HDR_BUFSZ];
                TraceEvents(TRACE_LEVEL_VERBOSE, FLAG_USBIP, "req %04x -> %Iu%s",
                        ptr04x(request), buf.Length, dbg_usbip_hdr(str, sizeof(str), &ctx->hdr, log_setup));
        }

        if (request && endpoint) {
                device::append_request(dev, *ctx, endpoint);
        }

        byteswap_header(ctx->hdr, swap_dir::host2net);

        auto wsk_irp = ctx->wsk_irp; // do not access ctx or wsk_irp after send
        IoSetCompletionRoutine(wsk_irp, send_complete, ctx.release(), true, true, true);

        NTSTATUS st;
        {
                wdf::Lock lck(dev.send_lock); // EvtUsbEndpointPurge, EvtIoInternalDeviceControl on other queues
                st = send(dev.sock(), &buf, WSK_FLAG_NODELAY, wsk_irp); // completion handler will be called anyway
        }
        TraceWSK("req %04x -> wsk irp %04x, %Iu bytes, %!STATUS!", 
                  ptr04x(request), ptr04x(wsk_irp), buf.Length, st);

        return STATUS_PENDING;
}

using urb_function_t = NTSTATUS (device_ctx&, UDECXUSBENDPOINT, endpoint_ctx&, WDFREQUEST, URB&);

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
auto control_transfer(
        _In_ device_ctx &dev, _In_ UDECXUSBENDPOINT endpoint, _In_ endpoint_ctx &endp,
        _In_ WDFREQUEST request, _In_ URB &urb)
{
        NT_ASSERT(usb_endpoint_type(endp.descriptor) == UsbdPipeTypeControl);

        static_assert(offsetof(_URB_CONTROL_TRANSFER, SetupPacket) == offsetof(_URB_CONTROL_TRANSFER_EX, SetupPacket));
        auto &r = urb.UrbControlTransferEx;

        if (r.PipeHandle && endp.PipeHandle != r.PipeHandle) { // r.PipeHandle is null if USBD_DEFAULT_PIPE_TRANSFER
                endp.PipeHandle = r.PipeHandle;
        }

        if (!filter::is_request(r)) {
                //
        } else if (auto func = filter::get_function(r, true); auto err = filter::unpack_request(dev, r, func)) {
                return err;
        }

        {
                char buf_flags[USBD_TRANSFER_FLAGS_BUFBZ];
                char buf_setup[USB_SETUP_PKT_STR_BUFBZ];

                TraceUrb("req %04x -> PipeHandle %04x, %s, TransferBufferLength %lu, Timeout %lu, %s",
                        ptr04x(request), ptr04x(r.PipeHandle),
                        usbd_transfer_flags(buf_flags, sizeof(buf_flags), r.TransferFlags),
                        r.TransferBufferLength,
                        urb.UrbHeader.Function == URB_FUNCTION_CONTROL_TRANSFER_EX ? r.Timeout : 0,
                        usb_setup_pkt_str(buf_setup, sizeof(buf_setup), r.SetupPacket));
        }

        auto buf_len = r.TransferBufferLength;
        auto &pkt = get_setup_packet(r); // @see UdecxUrbRetrieveControlSetupPacket

        if (buf_len > pkt.wLength) { // see drivers/usb/core/urb.c, usb_submit_urb
                buf_len = pkt.wLength; // usb_submit_urb checks for equality
        } else if (buf_len < pkt.wLength) {
                Trace(TRACE_LEVEL_ERROR, "TransferBufferLength(%lu) < wLength(%d)", buf_len, pkt.wLength);
                return STATUS_INVALID_PARAMETER;
        }

        wsk_context_ptr ctx(&dev, request);
        if (!ctx) {
                return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        setup_dir dir_out = is_transfer_dir_out(urb.UrbControlTransfer); // default control pipe is bidirectional

        if (auto err = set_cmd_submit_usbip_header(ctx->hdr, dev, endp.descriptor, r.TransferFlags, buf_len, dir_out)) {
                return err;
        }

        static_assert(sizeof(ctx->hdr.u.cmd_submit.setup) == sizeof(pkt));
        RtlCopyMemory(ctx->hdr.u.cmd_submit.setup, &pkt, sizeof(pkt));

        return send(endpoint, ctx, dev, true, &urb);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
auto bulk_or_interrupt_transfer(
        _In_ device_ctx &dev, _In_ UDECXUSBENDPOINT endpoint, _In_ endpoint_ctx &endp,
        _In_ WDFREQUEST request, _In_ URB &urb)
{
        NT_ASSERT(usb_endpoint_type(endp.descriptor) == UsbdPipeTypeBulk || 
                  usb_endpoint_type(endp.descriptor) == UsbdPipeTypeInterrupt);

        auto &r = urb.UrbBulkOrInterruptTransfer;

        if (endp.PipeHandle != r.PipeHandle) {
                endp.PipeHandle = r.PipeHandle;
        }

        {
                auto func = urb.UrbHeader.Function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER_USING_CHAINED_MDL ? ", MDL" : " ";
                char buf[USBD_TRANSFER_FLAGS_BUFBZ];

                TraceUrb("req %04x -> PipeHandle %04x, %s, TransferBufferLength %lu%s",
                        ptr04x(request), ptr04x(r.PipeHandle), usbd_transfer_flags(buf, sizeof(buf), r.TransferFlags),
                        r.TransferBufferLength, func);
        }

        wsk_context_ptr ctx(&dev, request);
        if (!ctx) {
                return STATUS_INSUFFICIENT_RESOURCES;
        }

        if (auto err = set_cmd_submit_usbip_header(ctx->hdr, dev, endp.descriptor, r.TransferFlags, r.TransferBufferLength)) {
                return err;
        }

        return send(endpoint, ctx, dev, false, &urb);
}

/*
 * USBD_ISO_PACKET_DESCRIPTOR.Length is not used (zero) for USB_DIR_OUT transfer.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
auto repack(_In_ usbip_iso_packet_descriptor *d, _In_ const _URB_ISOCH_TRANSFER &r)
{
        ULONG length = 0;

        for (ULONG i = 0; i < r.NumberOfPackets; ++d) {

                auto offset = r.IsoPacket[i].Offset;
                auto next_offset = ++i < r.NumberOfPackets ? r.IsoPacket[i].Offset : r.TransferBufferLength;

                if (next_offset >= offset && next_offset <= r.TransferBufferLength) {
                        d->offset = offset;
                        d->length = next_offset - offset;
                        d->actual_length = 0;
                        d->status = 0;
                        length += d->length;
                } else {
                        Trace(TRACE_LEVEL_ERROR, "[%lu] next_offset(%lu) >= offset(%lu) && next_offset <= r.TransferBufferLength(%lu)",
                                i, next_offset, offset, r.TransferBufferLength);
                        return STATUS_INVALID_PARAMETER;
                }
        }

        NT_ASSERT(length == r.TransferBufferLength);
        return STATUS_SUCCESS;
}

/*
 * USBD_START_ISO_TRANSFER_ASAP is appended because URB_GET_CURRENT_FRAME_NUMBER is not implemented.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
auto isoch_transfer(
        _In_ device_ctx &dev, _In_ UDECXUSBENDPOINT endpoint, _In_ endpoint_ctx &endp,
        _In_ WDFREQUEST request, _In_ URB &urb)
{
        NT_ASSERT(usb_endpoint_type(endp.descriptor) == UsbdPipeTypeIsochronous);
        auto &r = urb.UrbIsochronousTransfer;

        if (endp.PipeHandle != r.PipeHandle) {
                endp.PipeHandle = r.PipeHandle;
        }

        {
                const char *func = urb.UrbHeader.Function == URB_FUNCTION_ISOCH_TRANSFER_USING_CHAINED_MDL ? ", MDL" : " ";
                char buf[USBD_TRANSFER_FLAGS_BUFBZ];
                TraceUrb("req %04x -> PipeHandle %04x, %s, TransferBufferLength %lu, StartFrame %lu, NumberOfPackets %lu, ErrorCount %lu%s",
                        ptr04x(request), ptr04x(r.PipeHandle),
                        usbd_transfer_flags(buf, sizeof(buf), r.TransferFlags),
                        r.TransferBufferLength,
                        r.StartFrame,
                        r.NumberOfPackets,
                        r.ErrorCount,
                        func);
        }

        if (r.NumberOfPackets > USBIP_MAX_ISO_PACKETS) {
                Trace(TRACE_LEVEL_ERROR, "NumberOfPackets(%lu) > USBIP_MAX_ISO_PACKETS(%d)", 
                                          r.NumberOfPackets, USBIP_MAX_ISO_PACKETS);
                return STATUS_INVALID_PARAMETER;
        }

        wsk_context_ptr ctx(&dev, request, r.NumberOfPackets);
        if (!ctx) {
                return STATUS_INSUFFICIENT_RESOURCES;
        }

        if (auto err = set_cmd_submit_usbip_header(ctx->hdr, dev, endp.descriptor, 
                               r.TransferFlags | USBD_START_ISO_TRANSFER_ASAP, r.TransferBufferLength)) {
                return err;
        }

        if (auto err = repack(ctx->isoc, r)) {
                return err;
        }

        if (auto cmd = &ctx->hdr.u.cmd_submit) {
                cmd->start_frame = r.StartFrame;
                cmd->number_of_packets = r.NumberOfPackets;
        }

        return send(endpoint, ctx, dev, false, &urb);
}

/*
 * @see WdfRequestForwardToParentDeviceIoQueue
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
auto allocate_request_ctx(_In_ WDFREQUEST request)
{
        WDF_OBJECT_ATTRIBUTES attr;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, request_ctx); // as for WdfDeviceInitSetRequestAttributes

        if (auto err = WdfObjectAllocateContext(request, &attr, nullptr)) {
                Trace(TRACE_LEVEL_ERROR, "WdfObjectAllocateContext %!STATUS!", err);
                return err;
        }

        TraceDbg("%04x", ptr04x(request));
        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
auto usb_submit_urb(
        _In_ device_ctx &dev, _In_ UDECXUSBENDPOINT endpoint, _In_ endpoint_ctx &endp, _In_ WDFREQUEST request)
{
        if (get_request_ctx(request)) [[likely]] {
                // NULL for some devices
        } else if (auto err = allocate_request_ctx(request)) {
                return err;
        }

        auto &urb = get_urb(request);
        urb_function_t *handler{};

        switch (auto func = urb.UrbHeader.Function) {
        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER_USING_CHAINED_MDL:
                handler = bulk_or_interrupt_transfer;
                break;
        case URB_FUNCTION_ISOCH_TRANSFER:
        case URB_FUNCTION_ISOCH_TRANSFER_USING_CHAINED_MDL:
                handler = isoch_transfer;
                break;
        case URB_FUNCTION_CONTROL_TRANSFER_EX:
        case URB_FUNCTION_CONTROL_TRANSFER:
                handler = control_transfer;
                break;
        default:
                Trace(TRACE_LEVEL_ERROR, "%s(%#04x), dev %04x, endp %04x", urb_function_str(func), func, 
                                          ptr04x(endp.device), ptr04x(endpoint));

                return STATUS_NOT_SUPPORTED;
        }

        return handler(dev, endpoint, endp, request, urb);
}

/*
 * @param request can be WDF_NO_HANDLE
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
auto send_ep0_out(
        _In_ UDECXUSBDEVICE device, _In_opt_ WDFREQUEST request, _In_ const USB_DEFAULT_PIPE_SETUP_PACKET &setup)
{
        auto &dev = *get_device_ctx(device);

        wsk_context_ptr ctx(&dev, request);
        if (!ctx) {
                return STATUS_INSUFFICIENT_RESOURCES;
        }

        auto &ep0 = *get_endpoint_ctx(dev.ep0);
        const ULONG TransferFlags = USBD_DEFAULT_PIPE_TRANSFER | USBD_TRANSFER_DIRECTION_OUT;

        if (auto err = set_cmd_submit_usbip_header(ctx->hdr, dev, ep0.descriptor, TransferFlags, 0, setup_dir::out())) {
                return err;
        }

        if constexpr (auto &r = get_submit_setup(ctx->hdr); true) {
                r = setup;
                NT_ASSERT(!r.wLength);
                NT_ASSERT(is_transfer_dir_out(r));
        }

        return ::send(dev.ep0, ctx, dev, true);
}

} // namespace


 /*
  * There is a race condition between IRP cancelation and RET_SUBMIT.
  * Sequence of events:
  * 1.IRP is waiting for RET_SUBMIT in CSQ.
  * 2.An upper driver cancels IRP.
  * 3.IRP is removed from CSQ, IO_CSQ_COMPLETE_CANCELED_IRP callback is called.
  * 4.The callback inserts IRP into a list of unlinked IRPs (this step is imaginary).
  *
  * RET_SUBMIT can be received
  * a)Before #3 - normal case, IRP will be dequeued from CSQ.
  * b)Before #4 - IRP will not be found in CSQ and the list.
  * c)After #4 - IRP will be found in the list.
  *
  * Case b) is unavoidable because CSQ library calls IO_CSQ_COMPLETE_CANCELED_IRP after releasing a lock.
  * For that reason the cancellation logic is simplified and list of unlinked IRPs is not used.
  * RET_SUBMIT and RET_INLINK must be ignored if IRP is not found (IRP was cancelled and completed).
  */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void usbip::device::send_cmd_unlink_and_complete(_In_ UDECXUSBDEVICE device, _In_ WDFREQUEST request, _In_ NTSTATUS status)
{
        auto &dev = *get_device_ctx(device);
        auto &req = *get_request_ctx(request);

        TraceDbg("dev %04x, seqnum %u", ptr04x(device), req.seqnum);

        if (dev.unplugged) {
                TraceDbg("Unplugged, do not send unlink");
        } else if (auto ctx = wsk_context_ptr(&dev, WDFREQUEST(WDF_NO_HANDLE))) {
                set_cmd_unlink_usbip_header(ctx->hdr, dev, req.seqnum);
                ::send(WDF_NO_HANDLE, ctx, dev, false); // ignore error
        } else {
                Trace(TRACE_LEVEL_ERROR, "dev %04x, seqnum %u, wsk_context_ptr error", ptr04x(device), req.seqnum);
        }

        complete(request, status);
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
USB_DEFAULT_PIPE_SETUP_PACKET usbip::device::make_set_configuration(_In_ UCHAR ConfigurationValue)
{
        return USB_DEFAULT_PIPE_SETUP_PACKET {
                .bmRequestType{.B = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE},
                .bRequest = USB_REQUEST_SET_CONFIGURATION,
                .wValue{.W = ConfigurationValue},
        };
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
USB_DEFAULT_PIPE_SETUP_PACKET usbip::device::make_set_interface(
        _In_ UCHAR InterfaceNumber, _In_ UCHAR AlternateSetting)
{
        return USB_DEFAULT_PIPE_SETUP_PACKET {
                .bmRequestType{.B = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE},
                .bRequest = USB_REQUEST_SET_INTERFACE,
                .wValue{.W = AlternateSetting},
                .wIndex{.W = InterfaceNumber},
        };
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
USB_DEFAULT_PIPE_SETUP_PACKET usbip::device::make_clear_endpoint_stall(_In_ UCHAR EndpointAddress)
{
        return USB_DEFAULT_PIPE_SETUP_PACKET {
                .bmRequestType{.B = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT},
                .bRequest = USB_REQUEST_CLEAR_FEATURE,
                .wValue{.W = USB_FEATURE_ENDPOINT_STALL},
                .wIndex{.W = EndpointAddress},
        };
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
USB_DEFAULT_PIPE_SETUP_PACKET usbip::device::make_reset_port(_In_ USHORT port)
{
        static_assert(USB_RT_PORT == (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER));

        return USB_DEFAULT_PIPE_SETUP_PACKET {
                .bmRequestType{.B = USB_RT_PORT},
                .bRequest = USB_REQUEST_SET_FEATURE,
                .wValue{.W = USB_PORT_FEAT_RESET},
                .wIndex{.W = port},
        };
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS usbip::device::set_configuration(
        _In_ UDECXUSBDEVICE device, _In_opt_ WDFREQUEST request, _In_ UCHAR ConfigurationValue)
{
        TraceDbg("dev %04x, ConfigurationValue %d", ptr04x(device), ConfigurationValue);

        auto r = make_set_configuration(ConfigurationValue);
        return send_ep0_out(device, request, r);
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS usbip::device::set_interface(
        _In_ UDECXUSBDEVICE device, _In_opt_ WDFREQUEST request, 
        _In_ UCHAR InterfaceNumber, _In_ UCHAR AlternateSetting)
{
        TraceDbg("dev %04x, %d.%d", ptr04x(device), InterfaceNumber, AlternateSetting);

        auto r = make_set_interface(InterfaceNumber, AlternateSetting);
        return send_ep0_out(device, request, r);
}

/*
 * @see <linux>/drivers/usb/core/message.c, usb_clear_halt
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS usbip::device::clear_endpoint_stall(_In_ UDECXUSBENDPOINT endpoint, _In_opt_ WDFREQUEST request)
{
        auto &endp = *get_endpoint_ctx(endpoint);
        auto addr = endp.descriptor.bEndpointAddress;

        TraceDbg("dev %04x, endp %04x, bEndpointAddress %#x", ptr04x(endp.device), ptr04x(endpoint), addr);
 
        auto r = make_clear_endpoint_stall(addr);
        return send_ep0_out(endp.device, request, r);
}

/*
 * Call usb_reset_device on Linux side - warn interface drivers and perform a USB port reset.
 * @see <linux>/drivers/usb/usbip/stub_rx.c, is_reset_device_cmd, tweak_reset_device_cmd
 * @see <linux>/drivers/usb/usbip/vhci_hcd.c, vhci_hub_control
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS usbip::device::reset_port(_In_ UDECXUSBDEVICE device, _In_opt_ WDFREQUEST request)
{
        auto &dev = *get_device_ctx(device);

        NT_ASSERT(dev.port >= 1);
        auto port = static_cast<USHORT>(dev.port); // meaningless for a server which ignores it

        TraceDbg("dev %04x, port %d", ptr04x(device), port);

        auto r = make_reset_port(port);
        return send_ep0_out(device, request, r);
}

/*
 * IRP_MJ_INTERNAL_DEVICE_CONTROL 
 */
_Function_class_(EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void NTAPI usbip::device::internal_control(
        _In_ WDFQUEUE queue, 
        _In_ WDFREQUEST request,
        _In_ size_t /*OutputBufferLength*/,
        _In_ size_t /*InputBufferLength*/,
        _In_ ULONG IoControlCode)
{
        if (IoControlCode != IOCTL_INTERNAL_USB_SUBMIT_URB) {
                auto st = STATUS_INVALID_DEVICE_REQUEST;
                Trace(TRACE_LEVEL_ERROR, "%s(%#08lX) %!STATUS!", internal_device_control_name(IoControlCode), 
                                                                 IoControlCode, st);
                WdfRequestComplete(request, st);
                return;
        }

        auto endpoint = get_endpoint(queue);
        auto &endp = *get_endpoint_ctx(endpoint);
        
        if (auto dev = get_device_ctx(endp.device); dev->unplugged) {
                UdecxUrbComplete(request, USBD_STATUS_DEVICE_GONE);
        } else if (auto st = usb_submit_urb(*dev, endpoint, endp, request); st != STATUS_PENDING) {
                if (st) {
                        TraceDbg("%!STATUS!", st);
                }
                UdecxUrbCompleteWithNtStatus(request, st);
        }
}
