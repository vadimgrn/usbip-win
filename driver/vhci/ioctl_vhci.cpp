#include "ioctl_vhci.h"
#include "trace.h"
#include "ioctl_vhci.tmh"

#include "dbgcommon.h"
#include "vhci.h"
#include "plugin.h"
#include "vhub.h"
#include "ioctl_usrreq.h"

#include <usbuser.h>
#include <ntstrsafe.h>

namespace
{

/* 
 * The leading "\xxx\ " text is not included in the retrieved string.
 */
PAGEABLE ULONG get_name_prefix_cch(const UNICODE_STRING &s)
{
	PAGED_CODE();

	auto &str = s.Buffer;

	for (ULONG i = 1; *str == L'\\' && (i + 1)*sizeof(*str) <= s.Length; ++i) {
		if (str[i] == L'\\') {
			return i + 1;
		}
	}

	return 0;
}

} // namespace


PAGEABLE NTSTATUS vhub_get_roothub_name(vhub_dev_t *vhub, USB_ROOT_HUB_NAME &r, ULONG *poutlen)
{
	PAGED_CODE();

	auto &str = vhub->DevIntfRootHub;

	auto prefix_cch = get_name_prefix_cch(str);
	if (!prefix_cch) {
		Trace(TRACE_LEVEL_WARNING, "Prefix expected: DevIntfRootHub '%!USTR!'", &str);
	}

	ULONG str_sz = str.Length - prefix_cch*sizeof(*str.Buffer);
	ULONG r_sz = sizeof(r) - sizeof(*r.RootHubName) + str_sz;

	if (*poutlen < sizeof(r)) {
		*poutlen = r_sz;
		return STATUS_BUFFER_TOO_SMALL;
	}

	*poutlen = min(*poutlen, r_sz);

	r.ActualLength = r_sz;
	RtlStringCbCopyW(r.RootHubName, *poutlen - offsetof(USB_ROOT_HUB_NAME, RootHubName), str.Buffer + prefix_cch);
	
	TraceCall("ActualLength %lu, RootHubName '%S'", r.ActualLength, r.RootHubName);
	return STATUS_SUCCESS;
}

PAGEABLE NTSTATUS get_hcd_driverkey_name(vhci_dev_t *vhci, USB_HCD_DRIVERKEY_NAME &r, ULONG *poutlen)
{
	PAGED_CODE();

	ULONG prop_sz = 0;
	auto prop = get_device_prop(vhci->child_pdo->Self, DevicePropertyDriverKeyName, &prop_sz);
	if (!prop) {
		Trace(TRACE_LEVEL_ERROR, "Failed to get DevicePropertyDriverKeyName");
		return STATUS_UNSUCCESSFUL;
	}

	ULONG r_sz = sizeof(r) - sizeof(*r.DriverKeyName) + prop_sz;

	if (*poutlen < sizeof(r)) {
		*poutlen = r_sz;
		ExFreePoolWithTag(prop, USBIP_VHCI_POOL_TAG);
		return STATUS_BUFFER_TOO_SMALL;
	}

	*poutlen = min(*poutlen, r_sz);

	r.ActualLength = prop_sz;
	RtlStringCbCopyW(r.DriverKeyName, *poutlen - offsetof(USB_HCD_DRIVERKEY_NAME, DriverKeyName), prop);

	ExFreePoolWithTag(prop, USBIP_VHCI_POOL_TAG);

	TraceCall("ActualLength %lu, DriverKeyName '%S'", r.ActualLength, r.DriverKeyName);
	return STATUS_SUCCESS;
}

PAGEABLE NTSTATUS vhci_ioctl_vhci(vhci_dev_t *vhci, IO_STACK_LOCATION *irpstack, ULONG ioctl_code, void *buffer, ULONG inlen, ULONG *poutlen)
{
	PAGED_CODE();

	NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

	switch (ioctl_code) {
	case IOCTL_USBIP_VHCI_PLUGIN_HARDWARE:
		status = vhci_plugin_vpdo(vhci, static_cast<vhci_pluginfo_t*>(buffer), inlen, irpstack->FileObject);
		*poutlen = sizeof(vhci_pluginfo_t);
		break;
	case IOCTL_USBIP_VHCI_UNPLUG_HARDWARE:
		*poutlen = 0;
		status = inlen == sizeof(ioctl_usbip_vhci_unplug) ? 
			vhci_unplug_vpdo(vhci, static_cast<ioctl_usbip_vhci_unplug*>(buffer)->addr) :
			STATUS_INVALID_BUFFER_SIZE;
		break;
	case IOCTL_USBIP_VHCI_GET_PORTS_STATUS:
		status = vhub_get_ports_status(vhub_from_vhci(vhci), *static_cast<ioctl_usbip_vhci_get_ports_status*>(buffer), poutlen);
		break;
	case IOCTL_USBIP_VHCI_GET_IMPORTED_DEVICES:
		status = vhub_get_imported_devs(vhub_from_vhci(vhci), (ioctl_usbip_vhci_imported_dev*)buffer, 
						*poutlen/sizeof(ioctl_usbip_vhci_imported_dev));
		break;
	case IOCTL_GET_HCD_DRIVERKEY_NAME:
		status = get_hcd_driverkey_name(vhci, *static_cast<USB_HCD_DRIVERKEY_NAME*>(buffer), poutlen);
		break;
	case IOCTL_USB_GET_ROOT_HUB_NAME:
		status = vhub_get_roothub_name(vhub_from_vhci(vhci), *static_cast<USB_ROOT_HUB_NAME*>(buffer), poutlen);
		break;
	case IOCTL_USB_USER_REQUEST:
		status = vhci_ioctl_user_request(vhci, static_cast<USBUSER_REQUEST_HEADER*>(buffer), inlen, poutlen);
		break;
	default:
		Trace(TRACE_LEVEL_ERROR, "Unhandled %s(%#08lX)", dbg_ioctl_code(ioctl_code), ioctl_code);
	}

	return status;
}
