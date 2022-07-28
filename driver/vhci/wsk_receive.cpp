﻿/*
 * Copyright (C) 2022 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "wsk_receive.h"
#include <wdm.h>
#include "trace.h"
#include "wsk_receive.tmh"

#include "pdu.h"
#include "dev.h"
#include "usbd_helper.h"
#include "dbgcommon.h"
#include "urbtransfer.h"
#include "vpdo.h"
#include "csq.h"
#include "irp.h"
#include "network.h"
#include "wsk_context.h"
#include "vhub.h"
#include "vhci.h"
#include "internal_ioctl.h"

namespace
{

_IRQL_requires_max_(DISPATCH_LEVEL)
auto assign(ULONG &TransferBufferLength, int actual_length)
{
	bool ok = actual_length >= 0 && ULONG(actual_length) <= TransferBufferLength;
	TransferBufferLength = ok ? actual_length : 0;

	return ok ? STATUS_SUCCESS : STATUS_INVALID_BUFFER_SIZE;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS urb_function_generic(vpdo_dev_t&, URB &urb, const usbip_header &hdr)
{
	auto &r = AsUrbTransfer(urb);
	auto err = assign(r.TransferBufferLength, hdr.u.ret_submit.actual_length);

	if (err || !r.TransferBufferLength || is_transfer_direction_out(hdr)) { // TransferFlags can have wrong direction
		return err;
	}

//	auto func = urb.UrbHeader.Function;
//	auto log = func == URB_FUNCTION_CONTROL_TRANSFER || func == URB_FUNCTION_CONTROL_TRANSFER_EX; // don't expose sensitive data

//	if (!err && log) {
//		TraceUrb("%s(%#04x): %!BIN!", urb_function_str(func), func, WppBinary(buf, USHORT(r.TransferBufferLength)));
//	}

	return err;
}

/*
 * EP0 stall is not an error, control endpoint cannot stall.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS urb_select_configuration(vpdo_dev_t &vpdo, URB &urb, const usbip_header &hdr)
{
	auto err = urb.UrbHeader.Status;

	if (err == EndpointStalled) {
		Trace(TRACE_LEVEL_WARNING, "Ignoring EP0 %s, usbip status %d", get_usbd_status(err), hdr.u.ret_submit.status);
		err = USBD_STATUS_SUCCESS;
	}

	return err ? STATUS_UNSUCCESSFUL : vpdo_select_config(&vpdo, &urb.UrbSelectConfiguration);
}

/*
 * usb_set_interface can return -EPIPE, especially if a device's interface has only one altsetting.
 *
 * Note that control and isochronous endpoints don't halt, although control
 * endpoints report "protocol stall" (for unsupported requests) using the
 * same status code used to report a true stall.
 *
 * See: drivers/usb/core/message.c, usb_set_interface, usb_clear_halt.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS urb_select_interface(vpdo_dev_t &vpdo, URB &urb, const usbip_header &hdr)
{
	auto err = urb.UrbHeader.Status;

	if (err == EndpointStalled) {
		auto ifnum = urb.UrbSelectInterface.Interface.InterfaceNumber;
		Trace(TRACE_LEVEL_WARNING, "Ignoring EP0 %s, usbip status %d, InterfaceNumber %d, num_altsetting %d",
			get_usbd_status(err), hdr.u.ret_submit.status, ifnum,
			get_intf_num_altsetting(vpdo.actconfig, ifnum));

		err = USBD_STATUS_SUCCESS;
	}

	return err ? STATUS_UNSUCCESSFUL : vpdo_select_interface(&vpdo, &urb.UrbSelectInterface);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void cache_string_descriptor(
	_Inout_ vpdo_dev_t& vpdo, _In_ UCHAR index, _In_ USHORT lang_id, _In_ const USB_STRING_DESCRIPTOR &src)
{
	if (src.bLength == sizeof(USB_COMMON_DESCRIPTOR)) {
		TraceDbg("Skip empty string, index %d", index);
		return;
	}

	if (index >= ARRAYSIZE(vpdo.strings)) {
		TraceMsg("Can't save index %d in strings[%d]", index, ARRAYSIZE(vpdo.strings));
		return;
	}

	auto &dest = vpdo.strings[index];
	if (dest) {
		USHORT sz = src.bLength - offsetof(USB_STRING_DESCRIPTOR, bString);
		UNICODE_STRING str{ sz, sz, const_cast<WCHAR*>(src.bString) };
		if (index) {
			TraceDbg("strings[%d] -> '%!WSTR!', ignoring '%!USTR!'", index, dest->bString, &str);
		} else {
			TraceDbg("Ignoring list of supported languages");
		}
		return;
	}
	
	auto sz = src.bLength + sizeof(*src.bString); // + L'\0'

	auto sd = (USB_STRING_DESCRIPTOR*)ExAllocatePool2(POOL_FLAG_NON_PAGED | POOL_FLAG_UNINITIALIZED, sz, USBIP_VHCI_POOL_TAG);
	if (!sd) {
		Trace(TRACE_LEVEL_ERROR, "Can't allocate %Iu bytes", sz);
		return;
	}

	RtlCopyMemory(sd, &src, src.bLength);
	terminate_by_zero(*sd);
	dest = sd;

	if (index) {
		TraceMsg("Index %d, LangId %#x, '%!WSTR!'", index, lang_id, dest->bString);
	} else {
		TraceMsg("List of supported languages%!BIN!", WppBinary(dest, dest->bLength));
	}
}

/*
 * A request can read descriptor header or full descriptor to obtain its real size.
 * F.e. configuration descriptor is 9 bytes, but the full size is stored in wTotalLength.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS urb_control_descriptor_request(vpdo_dev_t &vpdo, URB &urb, const usbip_header &hdr)
{
	auto &r = urb.UrbControlDescriptorRequest;

	if (auto err = assign(r.TransferBufferLength, hdr.u.ret_submit.actual_length)) {
		return err;
	}

	if (is_transfer_direction_out(hdr)) { // TransferFlags can have wrong direction
		return STATUS_SUCCESS;
	}

	const USB_COMMON_DESCRIPTOR *dsc{};

	if (r.TransferBufferLength < sizeof(*dsc)) {
		Trace(TRACE_LEVEL_ERROR, "Descriptor header expected, TransferBufferLength(%lu)", r.TransferBufferLength);
		r.TransferBufferLength = 0;
		return STATUS_INVALID_BUFFER_SIZE;
	}

//	if (auto err = copy_to_transfer_buffer((void*&)dsc, vpdo, urb)) {
//		return err;
//	}

	TraceUrb("%s: bLength %d, %!usb_descriptor_type!, Index %d, LangId %#x %!BIN!",
		  urb_function_str(r.Hdr.Function), dsc->bLength, dsc->bDescriptorType, r.Index, r.LanguageId,
		  WppBinary(dsc, USHORT(r.TransferBufferLength)));

	switch (r.DescriptorType) {
	case USB_STRING_DESCRIPTOR_TYPE:
		if (dsc->bDescriptorType == USB_STRING_DESCRIPTOR_TYPE && dsc->bLength == r.TransferBufferLength) {
			auto &sd = *reinterpret_cast<const USB_STRING_DESCRIPTOR*>(dsc);
			auto &osd = *reinterpret_cast<const USB_OS_STRING_DESCRIPTOR*>(dsc);
			if (is_valid(osd)) {
				TraceMsg("MS_VendorCode %#x", osd.MS_VendorCode);
				vpdo.MS_VendorCode = osd.MS_VendorCode;
			} else if (is_valid(sd)) {
				cache_string_descriptor(vpdo, r.Index, r.LanguageId, sd);
			}
		}
		break;
	case USB_DEVICE_DESCRIPTOR_TYPE:
		if (!(r.TransferBufferLength == sizeof(vpdo.descriptor) && RtlEqualMemory(dsc, &vpdo.descriptor, sizeof(vpdo.descriptor)))) {
			Trace(TRACE_LEVEL_ERROR, "Device descriptor is not the same");
			vhub_unplug_vpdo(&vpdo);
		}
		break;
	}

	return STATUS_SUCCESS;
}

/*
 * Buffer from the server has no gaps (compacted), SUM(src->actual_length) == src_len,
 * src->offset is ignored for that reason.
 *
 * For isochronous packets: actual length is the sum of
 * the actual length of the individual, packets, but as
 * the packet offsets are not changed there will be
 * padding between the packets. To optimally use the
 * bandwidth the padding is not transmitted.
 *
 * See:
 * <linux>/drivers/usb/usbip/stub_tx.c, stub_send_ret_submit
 * <linux>/drivers/usb/usbip/usbip_common.c, usbip_pad_iso
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
auto copy_isoc_data(
	_Inout_ _URB_ISOCH_TRANSFER &r, _Out_ char *dst_buf,
	_Inout_ vpdo_dev_t &/*src_buf*/, _In_ ULONG src_len,
	_Out_ usbip_iso_packet_descriptor *sd, _In_ size_t /*sd_len*/)
{
	auto dir_out = !dst_buf;
//	auto sd_offset = dir_out ? 0 : src_len;

//	if (auto err = wsk_data_copy(src_buf, sd, sd_offset, sd_len)) {
//		Trace(TRACE_LEVEL_ERROR, "wsk_data_copy usbip_iso_packet_descriptor[%lu] %!STATUS!", r.NumberOfPackets, err);
//		return err;
//	}

	byteswap(sd, r.NumberOfPackets);

	ULONG src_offset = 0; // from src_buf
	auto dd = r.IsoPacket;

	for (ULONG i = 0; i < r.NumberOfPackets; ++i, ++dd, src_offset += sd++->actual_length) {

		dd->Status = sd->status ? to_windows_status_isoch(sd->status) : USBD_STATUS_SUCCESS;

		if (dir_out) {
			continue; // dd->Length is not used for OUT transfers
		}

		if (!sd->actual_length) {
			dd->Length = 0;
			continue;
		}

		if (sd->actual_length > sd->length) {
			Trace(TRACE_LEVEL_ERROR, "actual_length(%u) > length(%u)", sd->actual_length, sd->length);
			return STATUS_INVALID_PARAMETER;
		}

		if (sd->offset != dd->Offset) { // buffer is compacted, but offsets are intact
			Trace(TRACE_LEVEL_ERROR, "src.offset(%u) != dst.Offset(%lu)", sd->offset, dd->Offset);
			return STATUS_INVALID_PARAMETER;
		}

		if (src_offset > dd->Offset) {// source buffer has no gaps
			Trace(TRACE_LEVEL_ERROR, "src_offset(%lu) > dst.Offset(%lu)", src_offset, dd->Offset);
			return STATUS_INVALID_PARAMETER;
		}

		if (src_offset + sd->actual_length > src_len) {
			Trace(TRACE_LEVEL_ERROR, "src_offset(%lu) + src->actual_length(%u) > src_len(%lu)",
				                  src_offset, sd->actual_length, src_len);
			return STATUS_INVALID_PARAMETER;
		}

		if (dd->Offset + sd->actual_length > r.TransferBufferLength) {
			Trace(TRACE_LEVEL_ERROR, "dst.Offset(%lu) + src.actual_length(%u) > r.TransferBufferLength(%lu)",
				                  dd->Offset, sd->actual_length, r.TransferBufferLength);
			return STATUS_INVALID_PARAMETER;
		}

//		if (auto err = wsk_data_copy(src_buf, dst_buf + dd->Offset, src_offset, sd->actual_length)) {
//			Trace(TRACE_LEVEL_ERROR, "wsk_data_copy buffer[%lu] %!STATUS!", i, err);
//			return err;
//		}

		dd->Length = sd->actual_length;
	}

	bool ok = src_offset == src_len;
	if (!ok) {
		Trace(TRACE_LEVEL_ERROR, "src_offset(%lu) != src_len(%lu)", src_offset, src_len);
	}

	return ok ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

/*
 * Layout: usbip_header, transfer buffer(IN only), usbip_iso_packet_descriptor[].
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS urb_isoch_transfer(vpdo_dev_t &vpdo, URB &urb, const usbip_header &hdr)
{
	auto &res = hdr.u.ret_submit;
	auto cnt = res.number_of_packets;

	auto &r = urb.UrbIsochronousTransfer;
	r.ErrorCount = res.error_count;

	if (cnt && cnt == res.error_count) {
		r.Hdr.Status = USBD_STATUS_ISOCH_REQUEST_FAILED;
	}

	if (r.TransferFlags & USBD_START_ISO_TRANSFER_ASAP) {
		r.StartFrame = res.start_frame;
	}

	if (!(cnt >= 0 && ULONG(cnt) == r.NumberOfPackets)) {
		Trace(TRACE_LEVEL_ERROR, "number_of_packets(%d) != NumberOfPackets(%lu)", cnt, r.NumberOfPackets);
		return STATUS_INVALID_PARAMETER;
	}

	if (!(res.actual_length >= 0 && ULONG(res.actual_length) <= r.TransferBufferLength)) {
		Trace(TRACE_LEVEL_ERROR, "actual_length(%d) > TransferBufferLength(%lu)", res.actual_length, r.TransferBufferLength);
		return STATUS_INVALID_PARAMETER;
	}

	vpdo.current_frame_number = res.start_frame;

//	auto dir_in = is_transfer_direction_in(hdr); // TransferFlags can have wrong direction

//	auto dst_buf = dir_in ? (char*)get_urb_buffer(urb) : nullptr;
//	if (!dst_buf && dir_in) {
//		return STATUS_INSUFFICIENT_RESOURCES;
//	}

	if (auto ctx = alloc_wsk_context(r.NumberOfPackets)) {
//		auto err = copy_isoc_data(r, dst_buf, vpdo, res.actual_length, ctx->isoc, ctx->mdl_isoc.size());
//		free(ctx, false);
		return STATUS_NOT_IMPLEMENTED;
	} else {
		return STATUS_INSUFFICIENT_RESOURCES;
	}
}

/*
 * Nothing to handle.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS urb_function_success(vpdo_dev_t&, URB&, const usbip_header&)
{
	return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS urb_function_unexpected(vpdo_dev_t&, URB &urb, const usbip_header&)
{
	auto func = urb.UrbHeader.Function;
	Trace(TRACE_LEVEL_ERROR, "%s(%#04x) must never be called, internal logic error", urb_function_str(func), func);

	return STATUS_INTERNAL_ERROR;
}

using urb_function_t = NTSTATUS(vpdo_dev_t&, URB&, const usbip_header&);

urb_function_t* const urb_functions[] =
{
	urb_select_configuration,
	urb_select_interface,
	urb_function_unexpected, // URB_FUNCTION_ABORT_PIPE, urb_pipe_request

	urb_function_unexpected, // URB_FUNCTION_TAKE_FRAME_LENGTH_CONTROL
	urb_function_unexpected, // URB_FUNCTION_RELEASE_FRAME_LENGTH_CONTROL

	urb_function_unexpected, // URB_FUNCTION_GET_FRAME_LENGTH
	urb_function_unexpected, // URB_FUNCTION_SET_FRAME_LENGTH
	urb_function_unexpected, // URB_FUNCTION_GET_CURRENT_FRAME_NUMBER

	urb_function_generic, // URB_FUNCTION_CONTROL_TRANSFER
	urb_function_generic, // URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER
	urb_isoch_transfer,

	urb_control_descriptor_request, // URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE
	urb_control_descriptor_request, // URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE

	urb_function_success, // URB_FUNCTION_SET_FEATURE_TO_DEVICE, urb_control_feature_request
	urb_function_success, // URB_FUNCTION_SET_FEATURE_TO_INTERFACE, urb_control_feature_request
	urb_function_success, // URB_FUNCTION_SET_FEATURE_TO_ENDPOINT, urb_control_feature_request

	urb_function_success, // URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE, urb_control_feature_request
	urb_function_success, // URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE, urb_control_feature_request
	urb_function_success, // URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT, urb_control_feature_request

	urb_function_generic, // URB_FUNCTION_GET_STATUS_FROM_DEVICE
	urb_function_generic, // URB_FUNCTION_GET_STATUS_FROM_INTERFACE
	urb_function_generic, // URB_FUNCTION_GET_STATUS_FROM_ENDPOINT

	nullptr, // URB_FUNCTION_RESERVED_0X0016

	urb_function_generic, // URB_FUNCTION_VENDOR_DEVICE
	urb_function_generic, // URB_FUNCTION_VENDOR_INTERFACE
	urb_function_generic, // URB_FUNCTION_VENDOR_ENDPOINT

	urb_function_generic, // URB_FUNCTION_CLASS_DEVICE
	urb_function_generic, // URB_FUNCTION_CLASS_INTERFACE
	urb_function_generic, // URB_FUNCTION_CLASS_ENDPOINT

	nullptr, // URB_FUNCTION_RESERVE_0X001D

	urb_function_success, // URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL, urb_pipe_request

	urb_function_generic, // URB_FUNCTION_CLASS_OTHER
	urb_function_generic, // URB_FUNCTION_VENDOR_OTHER

	urb_function_generic, // URB_FUNCTION_GET_STATUS_FROM_OTHER

	urb_function_success, // URB_FUNCTION_SET_FEATURE_TO_OTHER, urb_control_feature_request
	urb_function_success, // URB_FUNCTION_CLEAR_FEATURE_TO_OTHER, urb_control_feature_request

	urb_control_descriptor_request, // URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT
	urb_control_descriptor_request, // URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT

	urb_function_generic, // URB_FUNCTION_GET_CONFIGURATION
	urb_function_generic, // URB_FUNCTION_GET_INTERFACE

	urb_control_descriptor_request, // URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE
	urb_control_descriptor_request, // URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE

	urb_function_generic, // URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR

	nullptr, // URB_FUNCTION_RESERVE_0X002B
	nullptr, // URB_FUNCTION_RESERVE_0X002C
	nullptr, // URB_FUNCTION_RESERVE_0X002D
	nullptr, // URB_FUNCTION_RESERVE_0X002E
	nullptr, // URB_FUNCTION_RESERVE_0X002F

	urb_function_unexpected, // URB_FUNCTION_SYNC_RESET_PIPE, urb_pipe_request
	urb_function_unexpected, // URB_FUNCTION_SYNC_CLEAR_STALL, urb_pipe_request
	urb_function_generic, // URB_FUNCTION_CONTROL_TRANSFER_EX

	nullptr, // URB_FUNCTION_RESERVE_0X0033
	nullptr, // URB_FUNCTION_RESERVE_0X0034

	urb_function_unexpected, // URB_FUNCTION_OPEN_STATIC_STREAMS
	urb_function_unexpected, // URB_FUNCTION_CLOSE_STATIC_STREAMS, urb_pipe_request
	urb_function_generic, // URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER_USING_CHAINED_MDL
	urb_isoch_transfer, // URB_FUNCTION_ISOCH_TRANSFER_USING_CHAINED_MDL

	nullptr, // 0x0039
	nullptr, // 0x003A
	nullptr, // 0x003B
	nullptr, // 0x003C

	urb_function_unexpected // URB_FUNCTION_GET_ISOCH_PIPE_TRANSFER_PATH_DELAYS
};

/*
 * If response from a server has data (actual_length > 0), URB function MUST copy it to URB
 * even if UrbHeader.Status != USBD_STATUS_SUCCESS.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS usb_submit_urb(vpdo_dev_t &vpdo, URB &urb, const usbip_header &hdr)
{
        {
                auto err = hdr.u.ret_submit.status;
                urb.UrbHeader.Status = err ? to_windows_status(err) : USBD_STATUS_SUCCESS;
        }

        auto func = urb.UrbHeader.Function;
        auto pfunc = func < ARRAYSIZE(urb_functions) ? urb_functions[func] : nullptr;

        auto err = pfunc ? pfunc(vpdo, urb, hdr) : STATUS_INVALID_PARAMETER;

        if (err && !urb.UrbHeader.Status) { // it's OK if (urb->UrbHeader.Status && !err)
                urb.UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
                Trace(TRACE_LEVEL_VERBOSE, "Set USBD_STATUS=%s because return is %!STATUS!", get_usbd_status(urb.UrbHeader.Status), err);
        }

        return err;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS usb_reset_port(const usbip_header &hdr)
{
        auto err = hdr.u.ret_submit.status;
        auto win_err = to_windows_status(err);

        if (win_err == EndpointStalled) { // control pipe stall is not an error, see urb_select_interface
                Trace(TRACE_LEVEL_WARNING, "Ignoring EP0 %s, usbip status %d", get_usbd_status(win_err), err);
                err = 0;
        }

        return err ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void ret_submit(vpdo_dev_t &vpdo, IRP *irp, const usbip_header &hdr)
{
	auto stack = IoGetCurrentIrpStackLocation(irp);
	auto &st = irp->IoStatus.Status;

        switch (auto ioctl = stack->Parameters.DeviceIoControl.IoControlCode) {
        case IOCTL_INTERNAL_USB_SUBMIT_URB:
		if (auto urb = static_cast<URB*>(URB_FROM_IRP(irp))) {
			st = usb_submit_urb(vpdo, *urb, hdr);
		}
                break;
        case IOCTL_INTERNAL_USB_RESET_PORT:
                st = usb_reset_port(hdr);
                break;
	default:
		Trace(TRACE_LEVEL_ERROR, "Unexpected IoControlCode %s(%#08lX)", dbg_ioctl_code(ioctl), ioctl);
		st = STATUS_INVALID_PARAMETER;
	}

	auto old_status = InterlockedCompareExchange(get_status(irp), ST_RECV_COMPLETE, ST_NONE);
	NT_ASSERT(old_status != ST_IRP_CANCELED);

	if (old_status == ST_SEND_COMPLETE) {
		TraceDbg("Complete irp %04x, %!STATUS!, Information %#Ix", ptr4log(irp), st, irp->IoStatus.Information);
		IoCompleteRequest(irp, IO_NO_INCREMENT);
	}
}

_IRQL_requires_max_(DISPATCH_LEVEL)
auto prepare_wsk_buf(_Out_ WSK_BUF &buf, _Inout_ wsk_context &ctx, _In_ const URB &urb, _In_ size_t length)
{
	if (is_transfer_direction_in(ctx.hdr)) { // TransferFlags can have wrong direction
		if (auto err = usbip::make_transfer_buffer_mdl(ctx.mdl_buf, IoWriteAccess, urb)) {
			Trace(TRACE_LEVEL_ERROR, "make_transfer_buffer_mdl %!STATUS!", err);
			return err;
		}
	}

	ctx.mdl_buf.next(ctx.is_isoc ? ctx.mdl_isoc.get() : nullptr);
//	byteswap(ctx.isoc, number_of_packets(ctx));

	buf.Mdl = ctx.mdl_buf.get();
	buf.Offset = 0;
	buf.Length = length;
	NT_ASSERT(buf.Length <= usbip::size(buf.Mdl));

	return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS on_read_payload(_In_ DEVICE_OBJECT*, _In_ IRP *wsk_irp, _In_reads_opt_(_Inexpressible_("varies")) void *Context)
{
	auto ctx = static_cast<wsk_context*>(Context);
	NT_ASSERT(ctx->wsk_irp == wsk_irp);

	auto &vpdo = *ctx->vpdo;
	auto &st = wsk_irp->IoStatus;

	TraceWSK("irql %!irql!, wsk irp %04x, %!STATUS!, Information %Iu",
		  KeGetCurrentIrql(), ptr4log(wsk_irp), st.Status, st.Information);

	if (NT_SUCCESS(st.Status)) {
		ret_submit(*ctx->vpdo, ctx->irp, ctx->hdr);
		sched_read_usbip_header(*ctx->vpdo);
	} else if (st.Status == STATUS_FILE_FORCED_CLOSED) {
		vhub_unplug_vpdo(&vpdo);
	}

	free(ctx);
	return StopCompletion;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void read_payload(_Inout_ wsk_context &ctx, _In_ size_t length)
{
	auto &urb = *static_cast<URB*>(URB_FROM_IRP(ctx.irp));

	WSK_BUF buf{};
	if (auto err = prepare_wsk_buf(buf, ctx, urb, length)) {
		Trace(TRACE_LEVEL_ERROR, "prepare_wsk_buf %!STATUS!", err);
		return;
	}

	auto wsk_irp = ctx.wsk_irp; // do not access ctx or wsk_irp after send
	IoSetCompletionRoutine(wsk_irp, on_read_payload, &ctx, true, true, true);

	auto err = receive(ctx.vpdo->sock, &buf, WSK_FLAG_WAITALL, wsk_irp);
	NT_ASSERT(err != STATUS_NOT_SUPPORTED);

	TraceWSK("wsk irp %04x, %!STATUS!", ptr4log(wsk_irp), err);
}

/*
 * For RET_UNLINK irp was completed before CMD_UNLINK was issued.
 * @see send_cmd_unlink
 *
 * USBIP_RET_UNLINK
 * 1) if UNLINK is successful, status is -ECONNRESET
 * 2) if USBIP_CMD_UNLINK is after USBIP_RET_SUBMIT status is 0
 * See: <kernel>/Documentation/usb/usbip_protocol.rst
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
void ret_command(_Inout_ vpdo_dev_t &vpdo, _Inout_ wsk_context &ctx)
{
	auto &hdr = ctx.hdr;
	auto irp = hdr.base.command == USBIP_RET_SUBMIT ? dequeue_irp(vpdo, hdr.base.seqnum) : nullptr;

	{
		char buf[DBG_USBIP_HDR_BUFSZ];
		TraceEvents(TRACE_LEVEL_VERBOSE, FLAG_USBIP, "irp %04x <- %Iu%s",
			    ptr4log(irp), get_total_size(hdr), dbg_usbip_hdr(buf, sizeof(buf), &hdr, false));
	}

	if (!irp) {
		// done
	} else if (auto sz = get_payload_size(hdr)) {
		ctx.irp = irp;
		read_payload(ctx, sz);
	} else {
		ret_submit(vpdo, irp, hdr);
	}
}

_IRQL_requires_max_(DISPATCH_LEVEL)
auto validate_header(_Inout_ usbip_header &hdr)
{
	auto &base = hdr.base;
	auto cmd = static_cast<usbip_request_type>(base.command);

	if (!(cmd == USBIP_RET_SUBMIT || cmd == USBIP_RET_UNLINK)) {
		Trace(TRACE_LEVEL_ERROR, "USBIP_RET_* expected, got %!usbip_request_type!", cmd);
		return false;
	}

	auto ok = is_valid_seqnum(base.seqnum);

	if (ok) {
		base.direction = extract_dir(base.seqnum); // always zero in server response
	} else {
		Trace(TRACE_LEVEL_ERROR, "Invalid seqnum %u", base.seqnum);
	}

	return ok;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS on_read_usbip_header(_In_ DEVICE_OBJECT*, _In_ IRP *wsk_irp, _In_reads_opt_(_Inexpressible_("varies")) void *Context)
{
	auto ctx = static_cast<wsk_context*>(Context);
	NT_ASSERT(ctx->wsk_irp == wsk_irp);

	auto &vpdo = *ctx->vpdo;
	auto &st = wsk_irp->IoStatus;

	TraceWSK("irql %!irql!, wsk irp %04x, %!STATUS!, Information %Iu",
		  KeGetCurrentIrql(), ptr4log(wsk_irp), st.Status, st.Information);

	if (NT_SUCCESS(st.Status)) {
		auto &hdr = ctx->hdr;
		NT_ASSERT(st.Information == sizeof(hdr));
		byteswap_header(hdr, swap_dir::net2host);
		if (validate_header(hdr)) {
			ret_command(vpdo, *ctx);
		}
	} else if (st.Status == STATUS_FILE_FORCED_CLOSED) {
		vhub_unplug_vpdo(&vpdo);
	}

	free(ctx);
	return StopCompletion;
}

_Function_class_(IO_WORKITEM_ROUTINE_EX)
_IRQL_requires_(PASSIVE_LEVEL)
_IRQL_requires_same_
void read_usbip_header(_In_ PVOID /*IoObject*/, _In_opt_ PVOID Context, _In_ PIO_WORKITEM IoWorkItem)
{
	IoFreeWorkItem(IoWorkItem);

	auto &ctx = *static_cast<wsk_context*>(Context);

	ctx.mdl_hdr.next(nullptr);
	WSK_BUF buf{ ctx.mdl_hdr.get(), 0, ctx.mdl_hdr.size() };

	auto wsk_irp = ctx.wsk_irp; // do not access ctx or wsk_irp after send
	IoSetCompletionRoutine(wsk_irp, on_read_usbip_header, &ctx, true, true, true);

	auto err = receive(ctx.vpdo->sock, &buf, WSK_FLAG_WAITALL, wsk_irp);
	NT_ASSERT(err != STATUS_NOT_SUPPORTED);

	TraceWSK("wsk irp %04x, %!STATUS!", ptr4log(wsk_irp), err);
}

} // namespace


_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS WskDisconnectEvent(_In_opt_ PVOID SocketContext, _In_ ULONG Flags)
{
	auto vpdo = static_cast<vpdo_dev_t*>(SocketContext);
	TraceMsg("vpdo %04x, Flags %#x", ptr4log(vpdo), Flags);

	vhub_unplug_vpdo(vpdo);
	return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS sched_read_usbip_header(_Inout_ vpdo_dev_t &vpdo)
{
	auto ctx = alloc_wsk_context(0);
	if (!ctx) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	ctx->vpdo = &vpdo;
	ctx->irp = nullptr;

	if (auto wi = IoAllocateWorkItem(vpdo.Self)) {
		const auto QueueType = static_cast<WORK_QUEUE_TYPE>(CustomPriorityWorkQueue + LOW_REALTIME_PRIORITY);
		IoQueueWorkItemEx(wi, read_usbip_header, QueueType, ctx);
		return STATUS_SUCCESS;
	} else {
		free(ctx);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
}
