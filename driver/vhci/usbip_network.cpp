/*
 * Copyright (C) 2022 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "usbip_network.h"
#include "trace.h"
#include "usbip_network.tmh"

#include "dev.h"
#include "pdu.h"
#include "dbgcommon.h"
#include "urbtransfer.h"
#include "usbip_proto.h"
#include "usbip_proto_op.h"
#include "usbd_helper.h"
#include "irp.h"

namespace
{

auto assign(ULONG &TransferBufferLength, int actual_length)
{
        bool ok = actual_length >= 0 && (ULONG)actual_length <= TransferBufferLength;
        TransferBufferLength = ok ? actual_length : 0;

        return ok ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

UINT32 get_request(UINT32 response)
{
        switch (static_cast<usbip_request_type>(response)) {
        case USBIP_RET_SUBMIT:
                return USBIP_CMD_SUBMIT;
        case USBIP_RET_UNLINK:
                return USBIP_CMD_UNLINK;
        default:
                return 0;
        }
}

auto recv_ret_submit(_In_ usbip::SOCKET *sock, _Inout_ URB &urb, _Inout_ usbip_header &hdr, _Inout_ usbip::Mdl &mdl_buf)
{
        auto &base = hdr.base;
        NT_ASSERT(base.command == USBIP_RET_SUBMIT);

        auto tr = AsUrbTransfer(&urb);

        {
                auto &ret = hdr.u.ret_submit;
                urb.UrbHeader.Status = ret.status ? to_windows_status(ret.status) : USBD_STATUS_SUCCESS;

                auto err = assign(tr->TransferBufferLength, ret.actual_length);
                if (err || base.direction == USBIP_DIR_OUT || !tr->TransferBufferLength) { 
                        return err;
                }
        }

        NT_ASSERT(mdl_buf.size() >= tr->TransferBufferLength);
        WSK_BUF buf{ mdl_buf.get(), 0, tr->TransferBufferLength };

        if (auto err = receive(sock, &buf)) {
                Trace(TRACE_LEVEL_ERROR, "Receive buffer[%Iu] %!STATUS!", buf.Length, err);
                return err;
        }

        TraceDbg("[%Iu]%!BIN!", buf.Length, WppBinary(mdl_buf.sysaddr(LowPagePriority), static_cast<USHORT>(buf.Length)));
        return STATUS_SUCCESS;
}

/*
 * URBs that are issued on DISPATCH_LEVEL have buffers from nonpaged pool.
 * TransferBufferLength can be zero.
 */ 
auto set_write_mdl_buffer(_Inout_ IRP *irp, _Inout_ URB &urb)
{
        auto &r = *AsUrbTransfer(&urb);

        auto &flags = get_flags(irp);
        auto irql = flags & F_IRQL_MASK;

        bool use_mdl = irql < DISPATCH_LEVEL && !r.TransferBufferMDL && r.TransferBuffer && r.TransferBufferLength;
        if (!use_mdl) {
                NT_ASSERT(!(flags & F_FREE_MDL));
                return STATUS_SUCCESS;
        }

        usbip::Mdl mdl;
        if (auto err = make_transfer_buffer_mdl(mdl, IoWriteAccess, urb)) {
                return err;
        }

        r.TransferBufferMDL = mdl.release();
        flags |= F_FREE_MDL;

        TraceDbg("irp %04x: TransferBufferMDL %04x", ptr4log(irp), ptr4log(r.TransferBufferMDL));
        return STATUS_SUCCESS;
}

} // namespace


NTSTATUS usbip::send(SOCKET *sock, memory pool, void *data, ULONG len)
{
        Mdl mdl(pool, data, len);
        if (auto err = mdl.prepare(IoReadAccess)) {
                return err;
        }

        WSK_BUF buf{ mdl.get(), 0, len };
        return send(sock, &buf, WSK_FLAG_NODELAY);
}

NTSTATUS usbip::recv(SOCKET *sock, memory pool, void *data, ULONG len)
{
        Mdl mdl(pool, data, len);
        if (auto err = mdl.prepare(IoWriteAccess)) {
                return err;
        }

        WSK_BUF buf{ mdl.get(), 0, len };
        return receive(sock, &buf);
}

err_t usbip::recv_op_common(_In_ SOCKET *sock, _In_ UINT16 expected_code, _Out_ op_status_t &status)
{
        op_common r;

        if (auto err = recv(sock, memory::stack, &r, sizeof(r))) {
                Trace(TRACE_LEVEL_ERROR, "Receive %!STATUS!", err);
                return ERR_NETWORK;
        }

	PACK_OP_COMMON(0, &r);

	if (r.version != USBIP_VERSION) {
		Trace(TRACE_LEVEL_ERROR, "Version(%#x) != expected(%#x)", r.version, USBIP_VERSION);
		return ERR_VERSION;
	}

        if (r.code != expected_code) {
                Trace(TRACE_LEVEL_ERROR, "Code(%#x) != expected(%#x)", r.code, expected_code);
                return ERR_PROTOCOL;
        }

        status = static_cast<op_status_t>(r.status);
        return ERR_NONE;
}

NTSTATUS usbip::send_cmd(_In_ SOCKET *sock, _Inout_ IRP *irp, _Inout_ usbip_header &hdr, _Inout_opt_ URB *transfer_buffer)
{
        usbip::Mdl mdl_hdr(memory::stack, &hdr, sizeof(hdr));

        if (auto err = mdl_hdr.prepare(IoReadAccess)) {
                Trace(TRACE_LEVEL_ERROR, "Prepare usbip_header %!STATUS!", err);
                return err;
        }

        usbip::Mdl buf_out;

        if (transfer_buffer) {
                auto &urb = *transfer_buffer;
                auto out = is_transfer_direction_out(&hdr);  // TransferFlags can have wrong direction
                if (auto err = out ? make_transfer_buffer_mdl(buf_out, IoReadAccess, urb) : set_write_mdl_buffer(irp, urb)) {
                        Trace(TRACE_LEVEL_ERROR, "make_buffer_mdl(%s) %!STATUS!", out ? "OUT" : "IN", err);
                        return err;
                }
        }

        mdl_hdr.next(buf_out);
        WSK_BUF buf{ mdl_hdr.get(), 0, get_total_size(hdr) };

        NT_ASSERT(buf.Length >= mdl_hdr.size());
        NT_ASSERT(buf.Length <= size(mdl_hdr)); // MDL for TransferBuffer can be larger than TransferBufferLength

        {
                char str[DBG_USBIP_HDR_BUFSZ];
                TraceEvents(TRACE_LEVEL_VERBOSE, FLAG_USBIP, "OUT %Iu%s", buf.Length, dbg_usbip_hdr(str, sizeof(str), &hdr, true));
        }

        byteswap_header(hdr, swap_dir::host2net);

        if (auto err = send(sock, &buf, WSK_FLAG_NODELAY)) {
                Trace(TRACE_LEVEL_ERROR, "Send %!STATUS!", err);
                return err;
        }

        return STATUS_SUCCESS;
}

/*
 * URB must have TransferBuffer* members.
 * TransferBuffer && TransferBufferMDL can be both not NULL for bulk/int at least.
 */
NTSTATUS usbip::make_transfer_buffer_mdl(_Out_ Mdl &mdl, _In_ LOCK_OPERATION Operation, _In_ const URB &urb)
{
        auto err = STATUS_SUCCESS;
        auto r = AsUrbTransfer(&urb);

        if (!r->TransferBufferLength) {
                NT_ASSERT(!mdl);
        } else if (auto m = r->TransferBufferMDL) {
                mdl = Mdl(m);
                err = mdl.size() >= r->TransferBufferLength ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
        } else if (auto buf = r->TransferBuffer) {
                mdl = Mdl(memory::paged, buf, r->TransferBufferLength); // FIXME: unknown if it is paged or not
                err = mdl.prepare_paged(Operation);
        } else {
                Trace(TRACE_LEVEL_ERROR, "TransferBuffer and TransferBufferMDL are NULL");
                err = STATUS_INVALID_PARAMETER;
        }

        if (err) {
                mdl.reset();
        }

        return err;
}

void usbip::free_transfer_buffer_mdl(_Inout_ IRP *irp)
{
        auto &flags = get_flags(irp);

        if (flags & F_FREE_MDL) {
                flags &= ~F_FREE_MDL;
        } else {
                return;
        }

        NT_ASSERT(IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB);
        auto urb = static_cast<URB*>(URB_FROM_IRP(irp));

        NT_ASSERT(has_transfer_buffer(*urb));
        auto &r = *AsUrbTransfer(urb);

        TraceDbg("irp %04x: TransferBufferMDL %04x", ptr4log(irp), ptr4log(r.TransferBufferMDL));
        NT_ASSERT(r.TransferBuffer && r.TransferBufferMDL);

        usbip::Mdl(r.TransferBufferMDL, usbip::mdl_type::paged); // unlock pages and release MDL
        r.TransferBufferMDL = nullptr;
}
