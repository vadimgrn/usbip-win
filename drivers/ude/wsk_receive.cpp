/*
 * Copyright (C) 2022 - 2024 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "wsk_receive.h"
#include "trace.h"
#include "wsk_receive.tmh"

#include "context.h"
#include "wsk_context.h"
#include "request_list.h"
#include "network.h"
#include "driver.h"
#include "ioctl.h"

#include <libdrv\usbd_helper.h>
#include <libdrv\dbgcommon.h>
#include <libdrv\usbdsc.h>
#include <libdrv\irp.h>
#include <libdrv\pdu.h>
#include <libdrv\ch9.h>

extern "C" {
#include <usbdlib.h>
}

namespace
{

using namespace usbip;

constexpr auto check(_In_ ULONG TransferBufferLength, _In_ int actual_length)
{
	return  actual_length >= 0 && static_cast<ULONG>(actual_length) <= TransferBufferLength ? 
		STATUS_SUCCESS : STATUS_INVALID_BUFFER_SIZE;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto assign(_Inout_ ULONG &TransferBufferLength, _In_ int actual_length)
{
	PAGED_CODE();
	auto err = check(TransferBufferLength, actual_length);
	TransferBufferLength = err ? 0 : actual_length;
	return err;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void log(_In_ const USB_DEVICE_DESCRIPTOR &d)
{
	PAGED_CODE();
	TraceUrb("DEV: bLength %d, bcdUSB %#x, bDeviceClass %#x, bDeviceSubClass %#x, bDeviceProtocol %#x, "
		"bMaxPacketSize0 %d, idVendor %#x, idProduct %#x, bcdDevice %#x, "
		"iManufacturer %d, iProduct %d, iSerialNumber %d, bNumConfigurations %d",
		d.bLength, d.bcdUSB, d.bDeviceClass, d.bDeviceSubClass, d.bDeviceProtocol, 
		d.bMaxPacketSize0, d.idVendor, d.idProduct, d.bcdDevice, 
		d.iManufacturer, d.iProduct, d.iSerialNumber, d.bNumConfigurations);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void log(_In_ const USB_CONFIGURATION_DESCRIPTOR &d)
{
	PAGED_CODE();
	TraceUrb("CFG: bLength %d, wTotalLength %hu(%#x), bNumInterfaces %d, "
		 "bConfigurationValue %d, iConfiguration %d, bmAttributes %#x, MaxPower %d",
		  d.bLength, d.wTotalLength, d.wTotalLength, d.bNumInterfaces, 
		  d.bConfigurationValue, d.iConfiguration, d.bmAttributes, d.MaxPower);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED inline auto& get_ret_submit(_In_ const wsk_context &ctx)
{
	PAGED_CODE();

	auto &hdr = ctx.hdr;
	NT_ASSERT(hdr.base.command == USBIP_RET_SUBMIT);
	return hdr.u.ret_submit;
}

/*
 * Isochronous transfers can only be used by full-speed and high-speed devices.
 * For devices and host controllers that can operate at full speed, the period is measured in units of 1 millisecond frames.
 * 
 * For devices and host controllers that can operate at high speed, the period is measured in units of microframes.
 * There are eight microframes in each 1 millisecond frame.
 * The period is related to the value in bInterval by the formula 2**(bInterval - 1), the result is number of microframes.
 * 
 * @param bInterval value for full-speed device, milliseconds
 * @return equivalent interval value for high-speed device
 * 
 * @see USB_ENDPOINT_DESCRIPTOR structure (usbspec.h)
 * @see 5.6.4 Isochronous Transfer Bus Access Constraints
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED UCHAR to_high_speed_interval(_In_ UCHAR bInterval)
{
	PAGED_CODE();
	NT_ASSERT(bInterval);

	if (bInterval == 1) {
		return 4; // 2**(4-1) = 8 microframes or 1ms
	} else if (bInterval < 4) {
		return 5; // 16mf or 2ms
	} else if (bInterval < 8) {
		return 6; // 32mf or 4ms
	} else if (bInterval < 16) {
		return 7; // 64mf or 8ms
	} else if (bInterval < 32) {
		return 8; // 128mf or 16ms
	} else { // up to 255
		return 9; // 256mf or 32ms
	}
}

/*
 * USB_SPEED_FULL audio devices do not work if ISOCH IN/OUT USB_ENDPOINT_DESCRIPTOR.bInterval = 1. 
 * ucx01000!UrbHandler_USBPORTStyle_Legacy_IsochTransfer completes IRP with USBD_STATUS_INVALID_PARAMETER,
 * this error can be observed in the filter driver, this driver will not get ISOCH transfers at all.
 *
 * UDE (perhaps due to its dependency on USBHUB3) does not support 1ms polling, 
 * it always treats bInterval as 0.125ms intervals, and it doesn't care 
 * if everything else in the device descriptor or speed is correct.
 * 
 * @see libdrv::find_next
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void fix_full_speed_endpoint_interval(_In_ USB_CONFIGURATION_DESCRIPTOR *cd)
{
	PAGED_CODE();

	for (auto cur = reinterpret_cast<USB_COMMON_DESCRIPTOR*>(cd); 
	     bool(cur = USBD_ParseDescriptors(cd, cd->wTotalLength, cur, USB_ENDPOINT_DESCRIPTOR_TYPE)); 
	     cur = libdrv::next(cur)) {

		auto &e = *reinterpret_cast<USB_ENDPOINT_DESCRIPTOR*>(cur);

		if (auto t = usb_endpoint_type(e); t == UsbdPipeTypeIsochronous || t == UsbdPipeTypeInterrupt) { // IN/OUT

			auto val = e.bInterval; // always treated as high-speed device despite it is full-speed
			e.bInterval = to_high_speed_interval(val);

			TraceDbg("bLength %d, %!usb_descriptor_type!, bEndpointAddress %#x, bmAttributes %#x, "
				 "wMaxPacketSize %d, bInterval %d (patched value is %d)", 
				  e.bLength, e.bDescriptorType, e.bEndpointAddress, e.bmAttributes, 
				  e.wMaxPacketSize, val, e.bInterval);
		}
	}
}

/*
 * Buffer from the server has no gaps (compacted), SUM(src->actual_length) == actual_length,
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
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto fill_isoc_data(_Inout_ _URB_ISOCH_TRANSFER &r, _In_opt_ UCHAR *buffer, _In_ ULONG length, 
	_In_ const usbip_iso_packet_descriptor *src)
{
	PAGED_CODE();

	NT_ASSERT(length <= r.TransferBufferLength);
	auto dir_out = !buffer;

	for (auto i = LONG64(r.NumberOfPackets) - 1; i >= 0; --i) { // set dd.Status and dd.Length

		auto sd = src + i;
		auto dd = r.IsoPacket + i;

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

		if (length >= sd->actual_length) {
			length -= sd->actual_length;
		} else {
			Trace(TRACE_LEVEL_ERROR, "length(%lu) >= actual_length(%u)", length, sd->actual_length);
			return STATUS_INVALID_PARAMETER;
		}

		if (dd->Offset + sd->actual_length > r.TransferBufferLength) {
			Trace(TRACE_LEVEL_ERROR, "dst.Offset(%lu) + src.actual_length(%u) > r.TransferBufferLength(%lu)",
				dd->Offset, sd->actual_length, r.TransferBufferLength);
			return STATUS_INVALID_PARAMETER;
		}
		
		if (dd->Offset < length) { // source buffer has no gaps
			Trace(TRACE_LEVEL_ERROR, "dst.Offset(%lu) < length(%lu)", dd->Offset, length);
			return STATUS_INVALID_PARAMETER;
		}

		if (dd->Offset > length) {
			RtlMoveMemory(buffer + dd->Offset, buffer + length, sd->actual_length);
		}

		dd->Length = sd->actual_length;
	}

	if (length && !dir_out) {
		Trace(TRACE_LEVEL_ERROR, "SUM(actual_length) != actual_length, delta is %lu", length);
		return STATUS_INVALID_PARAMETER; 
	}

	return STATUS_SUCCESS;
}

/*
 * Layout: transfer buffer(IN only), usbip_iso_packet_descriptor[].
 */
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
PAGED auto isoch_transfer(_In_ wsk_context &ctx, _In_ const usbip_header_ret_submit &ret, _Inout_ URB &urb)
{
	PAGED_CODE();
	auto cnt = ret.number_of_packets;

	auto &r = urb.UrbIsochronousTransfer;
	r.ErrorCount = ret.error_count;

	if (cnt && cnt == ret.error_count) {
		r.Hdr.Status = USBD_STATUS_ISOCH_REQUEST_FAILED;
	}

	if (r.TransferFlags & USBD_START_ISO_TRANSFER_ASAP) {
		r.StartFrame = ret.start_frame;
	}

	if (cnt >= 0 && ULONG(cnt) == r.NumberOfPackets) {
		NT_ASSERT(r.NumberOfPackets == number_of_packets(ctx));
		byteswap(ctx.isoc, cnt);
	} else {
		Trace(TRACE_LEVEL_ERROR, "number_of_packets(%d) != NumberOfPackets(%lu)", cnt, r.NumberOfPackets);
		return STATUS_INVALID_PARAMETER;
	}

	UCHAR *buffer{};

	if (is_transfer_dir_in(ctx.hdr)) { // TransferFlags can have wrong direction
		ULONG length; // full, not actual
		if (auto err = UdecxUrbRetrieveBuffer(ctx.request, &buffer, &length)) {
			Trace(TRACE_LEVEL_ERROR, "UdecxUrbRetrieveBuffer %!STATUS!", err);
			return err;
		}
	}

	return fill_isoc_data(r, buffer, ret.actual_length, ctx.isoc);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void complete_and_set_null(_Inout_ WDFREQUEST &request, _In_ NTSTATUS status)
{
	PAGED_CODE();
	complete(request, status);
	request = WDF_NO_HANDLE;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void post_control_transfer(_In_ const device_ctx &dev, _In_ const _URB_CONTROL_TRANSFER &r, _In_ void *TransferBuffer)
{
	PAGED_CODE();

	auto dsc = static_cast<USB_COMMON_DESCRIPTOR*>(TransferBuffer);
	auto dsc_len = static_cast<UINT16>(r.TransferBufferLength);

	auto ok = (r.TransferFlags & USBD_DEFAULT_PIPE_TRANSFER) &&
		is_transfer_dir_in(r) &&
		get_setup_packet(r).bRequest == USB_REQUEST_GET_DESCRIPTOR &&
		dsc_len >= sizeof(*dsc);

	if (!ok) {
		return;
	}

	TraceUrb("bLength %d, %!usb_descriptor_type!%!BIN!", 
		  dsc->bLength, dsc->bDescriptorType, WppBinary(dsc, dsc_len));

	switch (dsc->bDescriptorType) {
	case USB_CONFIGURATION_DESCRIPTOR_TYPE:
		if (auto &d = reinterpret_cast<USB_CONFIGURATION_DESCRIPTOR&>(*dsc);
		    dsc_len > sizeof(d) && d.bLength == sizeof(d) && d.wTotalLength == dsc_len) {
			NT_ASSERT(libdrv::is_valid(d));
			log(d);
			if (dev.speed() == USB_SPEED_FULL) {
				fix_full_speed_endpoint_interval(&d);
			}
		}
		break;
	case USB_DEVICE_DESCRIPTOR_TYPE:
		if (auto &d = reinterpret_cast<USB_DEVICE_DESCRIPTOR&>(*dsc); 
		    dsc_len == sizeof(d) && d.bLength == dsc_len) {
			NT_ASSERT(libdrv::is_valid(d));
			log(d);
		}
		break;
	}
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void post_process_transfer_buffer(_In_ const device_ctx &dev, _In_ const URB &urb, _In_ void *TransferBuffer)
{
	PAGED_CODE();

	switch (urb.UrbHeader.Function) {
	case URB_FUNCTION_CONTROL_TRANSFER_EX:
	case URB_FUNCTION_CONTROL_TRANSFER: // structures are binary compatible, see urbtransfer.cpp
		static_assert(sizeof(urb.UrbControlTransfer) == sizeof(urb.UrbControlTransferEx));
		post_control_transfer(dev, urb.UrbControlTransfer, TransferBuffer);
	}
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto ret_submit_urb(_Inout_ wsk_context &ctx, _In_ const usbip_header_ret_submit &ret, _Inout_ URB &urb)
{
	PAGED_CODE();
	urb.UrbHeader.Status = ret.status ? to_windows_status(ret.status) : USBD_STATUS_SUCCESS;

	if (is_isoch(urb)) {
		return isoch_transfer(ctx, ret, urb);
	}

	UCHAR *TransferBuffer{};
	ULONG TransferBufferLength{};

	if (auto err = UdecxUrbRetrieveBuffer(ctx.request, &TransferBuffer, &TransferBufferLength)) {
		return err == STATUS_INVALID_PARAMETER ? STATUS_SUCCESS : err; // OK if URB has no transfer buffer
	}

	auto st = STATUS_SUCCESS;

	if (TransferBufferLength != ULONG(ret.actual_length)) { // prepare_wsk_mdl can set it
		st = assign(TransferBufferLength, ret.actual_length); // DIR_OUT or !actual_length
		UdecxUrbSetBytesCompleted(ctx.request, TransferBufferLength);
	}

	if (NT_SUCCESS(st) && TransferBufferLength) {
		post_process_transfer_buffer(*ctx.dev, urb, TransferBuffer);
	}

	return st;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto ret_submit(_Inout_ wsk_context &ctx)
{
	PAGED_CODE();

	auto &ret = get_ret_submit(ctx);
	auto urb = try_get_urb(ctx.request); // IOCTL_INTERNAL_USB_SUBMIT_URB

	return  urb ? ret_submit_urb(ctx, ret, *urb) :
		ret.status ? STATUS_UNSUCCESSFUL : 
		STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto make_mdl_chain(_In_ wsk_context &ctx)
{
	PAGED_CODE();
	MDL *head{};

	if (!ctx.is_isoc) { // IN
		head = ctx.mdl_buf.get();
		NT_ASSERT(!head->Next);
	} else if (auto &chain = ctx.mdl_buf) { // isoch IN
		auto t = tail(chain);
		t->Next = ctx.mdl_isoc.get();
		head = chain.get();
	} else { // isoch OUT or IN with zero actual_length
		head = ctx.mdl_isoc.get();
	}

	return head;
}

/*
 * If response from a server has data (actual_length > 0), URB function MUST copy it to URB
 * even if UrbHeader.Status != USBD_STATUS_SUCCESS.
 * 
 * Ensure that URB has TransferBuffer and its size is sufficient.
 * Do others checks when payload will be read.
 * 
 * recv_payload -> prepare_wsk_mdl, there is payload to receive.
 * Payload layout:
 * a) DIR_IN: any type of transfer, [transfer_buffer] OR|AND [usbip_iso_packet_descriptor...]
 * b) DIR_OUT: ISOCH, <usbip_iso_packet_descriptor...>
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto prepare_wsk_mdl(_Out_ MDL* &mdl, _Inout_ wsk_context &ctx, _Inout_ URB &urb)
{
	PAGED_CODE();

	mdl = nullptr;
	auto &ret = get_ret_submit(ctx);

	if (auto err = prepare_isoc(ctx, ret.number_of_packets)) { // sets ctx.is_isoc
		return err;
	}

	UCHAR *TransferBuffer{};
	ULONG TransferBufferLength{};

	if (auto err = UdecxUrbRetrieveBuffer(ctx.request, &TransferBuffer, &TransferBufferLength)) { // URB must have transfer buffer
		Trace(TRACE_LEVEL_ERROR, "UdecxUrbRetrieveBuffer(%s) %!STATUS!", 
			                  urb_function_str(urb.UrbHeader.Function), err);
		return err;
	}

	auto dir_out = is_transfer_dir_out(ctx.hdr);
	bool fail{};

	if (ctx.is_isoc) { // always has payload
		fail = check(TransferBufferLength, ret.actual_length); // do not change buffer length
	} else { // actual_length MUST be assigned, must not have payload for OUT
		fail = assign(TransferBufferLength, ret.actual_length) || dir_out;
		UdecxUrbSetBytesCompleted(ctx.request, TransferBufferLength);
	}

	if (fail || !TransferBufferLength) {
		Trace(TRACE_LEVEL_ERROR, "TransferBufferLength(%lu), actual_length(%d), %!usbip_dir!", 
			                  TransferBufferLength, ret.actual_length, ctx.hdr.base.direction);
		return STATUS_INVALID_BUFFER_SIZE;
	}

	if (dir_out) {
		NT_ASSERT(ctx.is_isoc);
		NT_ASSERT(!ctx.mdl_buf);
	} else if (auto err = make_transfer_buffer_mdl(ctx.mdl_buf, ret.actual_length, IoWriteAccess, urb)) {
		Trace(TRACE_LEVEL_ERROR, "make_transfer_buffer_mdl %!STATUS!", err);
		return err;
	}

	mdl = make_mdl_chain(ctx);
	return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto receive(_Inout_ wsk_context &ctx, _Inout_ WSK_BUF &buf)
{
	PAGED_CODE();

	auto &dev = *ctx.dev;
	NT_ASSERT(verify(buf, ctx.is_isoc));

	SIZE_T actual{};
	auto st = receive(dev.sock(), &buf, WSK_FLAG_WAITALL, &actual);

	TraceWSK("req %04x, %!STATUS!, %Iu byte(s)", ptr04x(ctx.request), st, actual);

	return  NT_ERROR(st) ? st :
		actual == buf.Length ? STATUS_SUCCESS :
		actual ? STATUS_RECEIVE_PARTIAL : 
		STATUS_CONNECTION_DISCONNECTED; // EOF
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto drain_payload(_Inout_ wsk_context &ctx, _In_ size_t length)
{
	PAGED_CODE();

	if (ULONG(length) != length) {
		Trace(TRACE_LEVEL_ERROR, "Buffer size truncation: ULONG(%lu) != size_t(%Iu)", ULONG(length), length);
		return STATUS_INVALID_PARAMETER;
	}

	unique_ptr payload(libdrv::uninitialized, NonPagedPoolNx, length);

	if (auto ptr = payload.get()) {
		ctx.mdl_buf = Mdl(ptr, ULONG(length));
	} else {
		Trace(TRACE_LEVEL_ERROR, "Can't allocate %Iu bytes", length);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	if (auto err = ctx.mdl_buf.prepare_nonpaged()) {
		Trace(TRACE_LEVEL_ERROR, "prepare_nonpaged %!STATUS!", err);
		return err;
	}

	WSK_BUF buf{ .Mdl = ctx.mdl_buf.get(), .Length = length };
	return receive(ctx, buf);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto recv_payload(_Inout_ wsk_context &ctx, _In_ size_t length)
{
	PAGED_CODE();

	auto &urb = get_urb(ctx.request); // only IOCTL_INTERNAL_USB_SUBMIT_URB has payload
	WSK_BUF buf{ .Length = length };

	if (auto err = prepare_wsk_mdl(buf.Mdl, ctx, urb)) {
		Trace(TRACE_LEVEL_ERROR, "prepare_wsk_mdl %!STATUS!", err);
		return err;
	}

	return receive(ctx, buf);
}

/*
 * For RET_UNLINK irp was completed right after CMD_UNLINK was issued.
 * @see send_cmd_unlink
 *
 * USBIP_RET_UNLINK
 * 1) if UNLINK is successful, status is -ECONNRESET
 * 2) if USBIP_CMD_UNLINK is after USBIP_RET_SUBMIT status is 0
 * See: <kernel>/Documentation/usb/usbip_protocol.rst
 */
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
PAGED auto ret_command(_Inout_ wsk_context &ctx)
{
	PAGED_CODE();
	auto &hdr = ctx.hdr;

	auto request = hdr.base.command == USBIP_RET_SUBMIT ? // request must be completed
		       device::remove_request(*ctx.dev, hdr.base.seqnum) : WDF_NO_HANDLE;

	char buf[DBG_USBIP_HDR_BUFSZ];
	TraceEvents(TRACE_LEVEL_VERBOSE, FLAG_USBIP, "req %04x <- %Iu%s", ptr04x(request), 
		    get_total_size(hdr), dbg_usbip_hdr(buf, sizeof(buf), &hdr, false));

	return request;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto validate_header(_Inout_ usbip_header &hdr)
{
	PAGED_CODE();
	byteswap_header(hdr, swap_dir::net2host);

	auto &base = hdr.base;
	auto cmd = static_cast<usbip_request_type>(base.command);

	switch (cmd) {
	case USBIP_RET_SUBMIT: {
		auto &ret = hdr.u.ret_submit;
		if (ret.number_of_packets == number_of_packets_non_isoch) {
			ret.number_of_packets = 0;
		} else if (!is_valid_number_of_packets(ret.number_of_packets)) {
			Trace(TRACE_LEVEL_ERROR, "number_of_packets(%d) is out of range", ret.number_of_packets);
			return false;
		}
	}	break;
	case USBIP_RET_UNLINK:
		break;
	default:
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

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto recv_usbip_header(_Inout_ wsk_context &ctx)
{
	PAGED_CODE();

	ctx.mdl_buf.reset();
	ctx.mdl_hdr.next(nullptr);

	WSK_BUF buf{ .Mdl = ctx.mdl_hdr.get(), .Length = sizeof(ctx.hdr) };

	if (auto err = receive(ctx, buf)) {
		return err;
	}

	return validate_header(ctx.hdr) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void recv_loop(_Inout_ device_ctx &dev, _Inout_ wsk_context &ctx)
{
	PAGED_CODE();

	for (NTSTATUS status{}; !(status || dev.unplugged || recv_usbip_header(ctx)); ) {

		NT_ASSERT(!ctx.request); // must be completed and zeroed on every loop
		ctx.request = ret_command(ctx);

		if (auto sz = get_payload_size(ctx.hdr); !sz) {
			//
		} else if (dev.unplugged) {
			status = STATUS_CANCELLED; // do not receive payload
		} else {
			auto f = ctx.request ? recv_payload : drain_payload;
			status = f(ctx, sz);
		}

		if (auto &req = ctx.request) {
			auto st = status ? status : ret_submit(ctx);
			complete_and_set_null(req, st);
		}
	}
}

} // namespace


_IRQL_requires_same_
_Function_class_(KSTART_ROUTINE)
PAGED void usbip::recv_thread_function(_In_ void *context)
{
	PAGED_CODE();

	auto device = static_cast<UDECXUSBDEVICE>(context);
	TraceDbg("dev %04x", ptr04x(device));

	//KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
	auto dev = get_device_ctx(device);

	if (auto ctx = alloc_wsk_context(dev, WDF_NO_HANDLE)) {
		recv_loop(*dev, *ctx);
		NT_ASSERT(!ctx->request);
		free(ctx, true);
	}

	if (!dev->unplugged) {
		TraceDbg("dev %04x, detaching", ptr04x(device));
		device::detach(device);
	}

	TraceDbg("dev %04x, exited", ptr04x(device));
}

/*
 * To ensure compatibility with existing USB drivers, the UDE client must call WdfRequestComplete at DISPATCH_LEVEL.
 * @see Write a UDE client driver
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void usbip::complete(_In_ WDFREQUEST request, _In_ NTSTATUS status)
{
	auto irp = WdfRequestWdmGetIrp(request);

	auto info = irp->IoStatus.Information;
	NT_ASSERT(info == WdfRequestGetInformation(request));

	auto &req = *get_request_ctx(request);

	if (!libdrv::has_urb(irp)) {
		if (status) {
			TraceUrb("seqnum %u, %!STATUS!, Information %#Ix", req.seqnum, status, info);
		}
		libdrv::RaiseIrql lvl(DISPATCH_LEVEL);
		WdfRequestComplete(request, status);
		return;
	}

	auto &urb = *libdrv::urb_from_irp(irp);
	auto &urb_st = urb.UrbHeader.Status;

	if (status == STATUS_CANCELLED && urb_st == USBD_STATUS_PENDING) {
		urb_st = USBD_STATUS_CANCELED; // FIXME: is this really required?
	}

	if (status || urb_st) {
		TraceUrb("seqnum %u, USBD_%s, %!STATUS!, Information %#Ix", 
			  req.seqnum, get_usbd_status(urb_st), status, info);
	}

	auto endp = get_endpoint_ctx(req.endpoint);
	libdrv::RaiseIrql lvl(DISPATCH_LEVEL);

	if (auto boost = endp->priority_boost) {
		WdfRequestCompleteWithPriorityBoost(request, status, boost); // UdecxUrbComplete has no PriorityBoost
	} else {
		UdecxUrbCompleteWithNtStatus(request, status);
		static_assert(!IO_NO_INCREMENT);
	}
}

