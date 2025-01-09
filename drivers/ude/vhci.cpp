/*
 * Copyright (C) 2022 - 2024 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "vhci.h"
#include "trace.h"
#include "vhci.tmh"

#include "persistent.h"
#include "vhci_ioctl.h"

#include <ntstrsafe.h>

#include <usbdlib.h>
#include <usbiodef.h>

namespace
{

using namespace usbip;

/*
 * WDF calls the callback at PASSIVE_LEVEL if object's handle type is WDFDEVICE.
 */
_Function_class_(EVT_WDF_DEVICE_CONTEXT_CLEANUP)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
PAGED void vhci_cleanup(_In_ WDFOBJECT object)
{
        PAGED_CODE();

        auto vhci = static_cast<WDFDEVICE>(object);
        TraceDbg("vhci %04x", ptr04x(vhci));
}

_Function_class_(EVT_WDF_IO_QUEUE_IO_CANCELED_ON_QUEUE)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void NTAPI canceled_on_queue(_In_ WDFQUEUE, _In_ WDFREQUEST request)
{
        TraceDbg("read request %04x", ptr04x(request));
        WdfRequestComplete(request, STATUS_CANCELLED);
}

using init_func_t = NTSTATUS(WDFDEVICE);

_Function_class_(init_func_t)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto init_context(_In_ WDFDEVICE vhci)
{
        PAGED_CODE();
        auto &ctx = *get_vhci_ctx(vhci);

        WDF_OBJECT_ATTRIBUTES attr;
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = vhci;

        if (auto err = WdfSpinLockCreate(&attr, &ctx.devices_lock)) {
                Trace(TRACE_LEVEL_ERROR, "WdfSpinLockCreate %!STATUS!", err);
                return err;
        }

        return STATUS_SUCCESS;
}

_Function_class_(init_func_t)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto create_interfaces(_In_ WDFDEVICE vhci)
{
        PAGED_CODE();

        const GUID* v[] = {
                &GUID_DEVINTERFACE_USB_HOST_CONTROLLER,
                &vhci::GUID_DEVINTERFACE_USB_HOST_CONTROLLER
        };

        for (auto guid: v) {
                if (auto err = WdfDeviceCreateDeviceInterface(vhci, guid, nullptr)) {
                        Trace(TRACE_LEVEL_ERROR, "WdfDeviceCreateDeviceInterface(%!GUID!) %!STATUS!", guid, err);
                        return err;
                }
        }

        return STATUS_SUCCESS;
}

_Function_class_(EVT_UDECX_WDF_DEVICE_QUERY_USB_CAPABILITY)
_IRQL_requires_same_
NTSTATUS query_usb_capability(
        _In_ WDFDEVICE /*UdecxWdfDevice*/,
        _In_ GUID *,
        _In_ ULONG /*OutputBufferLength*/,
        _Out_writes_to_opt_(OutputBufferLength, *ResultLength) PVOID /*OutputBuffer*/,
        _Out_ ULONG *)
{
        return STATUS_NOT_SUPPORTED;
}

/*
 * If TargetState is WdfPowerDeviceD3Final, you should assume that the system is being turned off, 
 * the device is about to be removed, or a resource rebalance is in progress.
 * 
 * Cannot be used for actions that are done in EVT_WDF_DEVICE_QUERY_REMOVE 
 * because if the device is in D1-3 state, this callback will not be called again. 
 * The second reason is that if something (app, driver) holds a reference to WDFDEVICE, 
 * EVT_WDF_DEVICE_D0_EXIT(WdfPowerDeviceD3Final) will not be called.
 */
_Function_class_(EVT_WDF_DEVICE_D0_EXIT)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
PAGED NTSTATUS NTAPI vhci_d0_exit(_In_ WDFDEVICE, _In_ WDF_POWER_DEVICE_STATE TargetState)
{
        PAGED_CODE();
        TraceDbg("TargetState %!WDF_POWER_DEVICE_STATE!", TargetState);
        return STATUS_SUCCESS;
}

_Function_class_(EVT_WDF_DEVICE_D0_ENTRY)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
PAGED NTSTATUS NTAPI vhci_d0_entry(_In_ WDFDEVICE, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
        PAGED_CODE();
        TraceDbg("PreviousState %!WDF_POWER_DEVICE_STATE!", PreviousState);
        return STATUS_SUCCESS;
}

/* 
 * This callback determines whether a specified device can be stopped and removed.
 * The framework does not synchronize the EvtDeviceQueryRemove callback function 
 * with other PnP and power management callback functions.
 * 
 * VHCI device will not be removed until all FILEOBJECT-s will be closed.
 * The uninstaller will block on the command that removes VHCI device node.
 * Cancelling read requests forces apps to close handle of VHCI device.
 * 
 * FIXME: can be called several times (if IRP_MN_CANCEL_REMOVE_DEVICE was issued?).
 */
_Function_class_(EVT_WDF_DEVICE_QUERY_REMOVE)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
PAGED NTSTATUS vhci_query_remove(_In_ WDFDEVICE vhci)
{
        PAGED_CODE();
        TraceDbg("%04x", ptr04x(vhci));

        //detach_all_devices(vhci, vhci::detach_call::async_nowait); // must not block this callback for long time
        return STATUS_SUCCESS;
}

/*
 * Drivers for USB devices must not specify IdleCanWakeFromS0. 
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto initialize(_Inout_ WDFDEVICE_INIT *init)
{
        PAGED_CODE();

        {
                WDF_PNPPOWER_EVENT_CALLBACKS cb;
                WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&cb);

                //cb.EvtDeviceD0Exit = vhci_d0_exit;
                //cb.EvtDeviceD0Entry = vhci_d0_entry;
                //cb.EvtDeviceQueryRemove = vhci_query_remove;

                WdfDeviceInitSetPnpPowerEventCallbacks(init, &cb);
        }

        {
                //WDF_REMOVE_LOCK_OPTIONS opts;
                //WDF_REMOVE_LOCK_OPTIONS_INIT(&opts, WDF_REMOVE_LOCK_OPTION_ACQUIRE_FOR_IO);
                //WdfDeviceInitSetRemoveLockOptions(init, &opts);
        }

        {
                WDF_OBJECT_ATTRIBUTES attr;
                WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, request_ctx);
                WdfDeviceInitSetRequestAttributes(init, &attr);
        }

        WdfDeviceInitSetCharacteristics(init, FILE_AUTOGENERATED_DEVICE_NAME, true);

        if (auto err = WdfDeviceInitAssignSDDLString(init, &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R)) {
                Trace(TRACE_LEVEL_ERROR, "WdfDeviceInitAssignSDDLString %!STATUS!", err);
                return err;
        }

        if (auto err = UdecxInitializeWdfDeviceInit(init)) {
                Trace(TRACE_LEVEL_ERROR, "UdecxInitializeWdfDeviceInit %!STATUS!", err);
                return err;
        }

        return STATUS_SUCCESS;
}

_Function_class_(init_func_t)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto add_usbdevice_emulation(_In_ WDFDEVICE vhci)
{
        PAGED_CODE();

        UDECX_WDF_DEVICE_CONFIG cfg;
        UDECX_WDF_DEVICE_CONFIG_INIT(&cfg, query_usb_capability);

        cfg.NumberOfUsb20Ports = USB2_PORTS;
        cfg.NumberOfUsb30Ports = USB3_PORTS;

        if (auto err = UdecxWdfDeviceAddUsbDeviceEmulation(vhci, &cfg)) {
                Trace(TRACE_LEVEL_ERROR, "UdecxWdfDeviceAddUsbDeviceEmulation %!STATUS!", err);
                return err;
        }

        return STATUS_SUCCESS;
}

_Function_class_(init_func_t)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto configure(_In_ WDFDEVICE vhci)
{
        PAGED_CODE();

        {
                WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS idle_settings;
                WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&idle_settings, IdleCannotWakeFromS0);

                if (auto err = WdfDeviceAssignS0IdleSettings(vhci, &idle_settings)) {
                        Trace(TRACE_LEVEL_ERROR, "WdfDeviceAssignS0IdleSettings %!STATUS!", err);
                        return err;
                }
        }

/*
        {
                WDF_DEVICE_POWER_POLICY_WAKE_SETTINGS wake;
                WDF_DEVICE_POWER_POLICY_WAKE_SETTINGS_INIT(&wake);
                wake.

                if (auto err = WdfDeviceAssignSxWakeSettings(vhci, &wake)) {
                        Trace(TRACE_LEVEL_ERROR, "WdfDeviceAssignSxWakeSettings %!STATUS!", err);
                        return err;
                }
        }

        {
                WDF_DEVICE_POWER_CAPABILITIES caps;
                WDF_DEVICE_POWER_CAPABILITIES_INIT(&caps);
                WdfDeviceSetPowerCapabilities(vhci, &caps);
        }

        {
                WDF_DEVICE_PNP_CAPABILITIES caps;
                WDF_DEVICE_PNP_CAPABILITIES_INIT(&caps);
                WdfDeviceSetPnpCapabilities(vhci, &caps);
        }
*/
        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto create_vhci(_Out_ WDFDEVICE &vhci, _In_ WDFDEVICE_INIT *init)
{
        PAGED_CODE();

        WDF_OBJECT_ATTRIBUTES attr; // default parent (WDFDRIVER) is OK
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, vhci_ctx);
        attr.EvtCleanupCallback = vhci_cleanup;

        if (auto err = WdfDeviceCreate(&init, &attr, &vhci)) {
                Trace(TRACE_LEVEL_ERROR, "WdfDeviceCreate %!STATUS!", err);
                return err;
        }

        init_func_t* const functions[] { init_context, configure, create_interfaces, 
                                         add_usbdevice_emulation, vhci::create_queues };

        for (auto f: functions) {
                if (auto err = f(vhci)) {
                        return err;
                }
        }

        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
auto get_port_range(_In_ usb_device_speed speed)
{
        struct{ int begin;  int end; } r;

        if (speed < USB_SPEED_SUPER) {
                r.begin = 0;
                r.end = USB2_PORTS;
        } else {
                r.begin = USB2_PORTS;
                r.end = TOTAL_PORTS;
        }

        return r;
}

} // namespace


/*
 * usb2.0 devices don't work in usb3.x ports, and visa versa, tested.
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
int usbip::vhci::claim_roothub_port(_In_ UDECXUSBDEVICE device)
{
        auto &dev = *get_device_ctx(device);
        auto &vhci = *get_vhci_ctx(dev.vhci); 

        NT_ASSERT(!dev.port);
        int port = 0;

        auto [begin, end] = get_port_range(dev.speed());

        wdf::Lock lck(vhci.devices_lock); // function must be resident, do not use PAGED

        for (auto i = begin; i < end; ++i) {
                NT_ASSERT(i < ARRAYSIZE(vhci.devices));

                if (auto &handle = vhci.devices[i]; !handle) {
                        WdfObjectReference(handle = device);
                        
                        port = i + 1;
                        NT_ASSERT(is_valid_port(port));

                        dev.port = port;
                        break;
                }
        }

        lck.release();
        return port;
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
int usbip::vhci::reclaim_roothub_port(_In_ UDECXUSBDEVICE device)
{
        auto &dev = *get_device_ctx(device);
        auto &vhci = *get_vhci_ctx(dev.vhci); 

        static_assert(!is_valid_port(0));
        int portnum = 0;

        wdf::Lock lck(vhci.devices_lock); 
        if (auto &port = dev.port) {
                NT_ASSERT(is_valid_port(port));
                portnum = port;

                auto &handle = vhci.devices[port - 1];
                NT_ASSERT(handle == device);

                handle = WDF_NO_HANDLE;
                port = 0;
        }
        lck.release(); // explicit call to satisfy code analyzer and get rid of warning C28166

        if (portnum) {
                WdfObjectDereference(device);
        }
        
        return portnum;
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
wdf::ObjectRef usbip::vhci::get_device(_In_ WDFDEVICE vhci, _In_ int port)
{
        wdf::ObjectRef ptr;
        if (!is_valid_port(port)) {
                return ptr;
        }

        auto &ctx = *get_vhci_ctx(vhci);

        wdf::Lock lck(ctx.devices_lock); 
        if (auto handle = ctx.devices[port - 1]) {
                NT_ASSERT(get_device_ctx(handle)->port == port);
                ptr.reset(handle); // adds reference
        }
        lck.release(); // explicit call to satisfy code analyzer and get rid of warning C28166

        return ptr;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void usbip::vhci::detach_all_devices(_In_ WDFDEVICE , _In_ detach_call )
{
        PAGED_CODE();
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::vhci::fill(_Out_ imported_device &dev, _In_ const device_ctx_ext &ext, _In_ int port)
{
        PAGED_CODE();

//      imported_device_location
        dev.port = port;
        if (auto err = copy(dev.host, sizeof(dev.host), ext.node_name, 
                            dev.service, sizeof(dev.service), ext.service_name,  
                            dev.busid, sizeof(dev.busid), ext.busid)) {
                return err;
        }
//
        static_cast<imported_device_properties&>(dev) = ext.dev;
        return STATUS_SUCCESS;
}

/*
 * Drivers cannot call WdfObjectDelete to delete WDFDEVICE.
 * WdfObjectDelete: Attempt to Delete an Object Which does not allow WdfDeleteObject, STATUS_CANNOT_DELETE.
 */
_Function_class_(EVT_WDF_DRIVER_DEVICE_ADD)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::DeviceAdd(_In_ WDFDRIVER, _Inout_ WDFDEVICE_INIT *init)
{
        PAGED_CODE();

        if (auto err = initialize(init)) {
                return err;
        }

        WDFDEVICE vhci{};
        if (auto err = create_vhci(vhci, init)) { 
                // the framework handles deletion of WDFDEVICE
                return err;
        }

        Trace(TRACE_LEVEL_INFORMATION, "vhci %04x", ptr04x(vhci));
        return STATUS_SUCCESS;
}
