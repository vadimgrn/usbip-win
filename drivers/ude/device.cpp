/*
 * Copyright (C) 2022 - 2024 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "device.h"
#include "trace.h"
#include "device.tmh"

#include "driver.h"
#include "request_list.h"
#include "endpoint_list.h"
#include "network.h"
#include "device_ioctl.h"
#include "wsk_receive.h"
#include "ioctl.h"
#include "vhci.h"

#include <libdrv/dbgcommon.h>
#include <libdrv/wait_timeout.h>

namespace
{

using namespace usbip;

_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
PAGED auto to_udex_speed(_In_ usb_device_speed speed)
{
        PAGED_CODE();

        switch (speed) {
        case USB_SPEED_SUPER_PLUS:
        case USB_SPEED_SUPER:
                return UdecxUsbSuperSpeed;
        case USB_SPEED_WIRELESS:
        case USB_SPEED_HIGH:
                return UdecxUsbHighSpeed;
        case USB_SPEED_FULL:
                return UdecxUsbFullSpeed;
        case USB_SPEED_LOW:
                return UdecxUsbLowSpeed;
        case USB_SPEED_UNKNOWN:
        default:
                return UdecxUsbHighSpeed; // FIXME
        }
}

_Function_class_(EVT_WDF_DEVICE_CONTEXT_DESTROY)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
PAGED void NTAPI device_destroy(_In_ WDFOBJECT object)
{
        PAGED_CODE();

        auto device = static_cast<UDECXUSBDEVICE>(object);
        auto &ext = get_device_ctx(device)->ext;

        Trace(TRACE_LEVEL_INFORMATION, "dev %04x, %!USTR!:%!USTR!/%!USTR!", 
                ptr04x(device), &ext->node_name, &ext->service_name, &ext->busid);

        free(ext);
        ext = nullptr;
}

_Function_class_(EVT_WDF_DEVICE_CONTEXT_CLEANUP)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
PAGED void device_cleanup(_In_ WDFOBJECT Object)
{
        PAGED_CODE();

        auto device = static_cast<UDECXUSBDEVICE>(Object);
        auto &dev = *get_device_ctx(device);

        Trace(TRACE_LEVEL_INFORMATION, "dev %04x, cancelable(%!UINT64!) / sent(%!UINT64!) requests",
                ptr04x(device), dev.cancelable_requests, dev.sent_requests);

        // all resources must be freed except for device_ctx_ext*
        NT_ASSERT(IsListEmpty(&dev.requests));
        NT_ASSERT(dev.unplugged);
        NT_ASSERT(!dev.port);
        NT_ASSERT(!dev.recv_thread);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED _KTHREAD *recv_thread_join(_In_ UDECXUSBDEVICE device, _Inout_ device_ctx &dev)
{
        PAGED_CODE();

        auto thread = (_KTHREAD*)InterlockedExchangePointer(reinterpret_cast<PVOID*>(&dev.recv_thread), nullptr);
        NT_ASSERT(thread);

        if (thread == KeGetCurrentThread()) {
                return thread;
        }

        TraceDbg("dev %04x", ptr04x(device));
        NT_VERIFY(!KeWaitForSingleObject(thread, Executive, KernelMode, false, nullptr));
        TraceDbg("dev %04x, joined", ptr04x(device));

        ObDereferenceObject(thread);
        return nullptr;
}

/*
 * @see UDECX_WDF_DEVICE_CONFIG.UDECX_WDF_DEVICE_RESET_ACTION, 
 *      default is UdecxWdfDeviceResetActionResetEachUsbDevice. 
 */
_Function_class_(EVT_UDECX_USB_DEVICE_POST_ENUMERATION_RESET)
_IRQL_requires_same_
inline void device_reset(
        _In_ WDFDEVICE vhci, _In_ UDECXUSBDEVICE dev, _In_ WDFREQUEST request, _In_ BOOLEAN AllDevicesReset)
{
        if (AllDevicesReset) {
                Trace(TRACE_LEVEL_ERROR, "vhci %04x, dev %04x, AllDevicesReset(true) is not supported", 
                        ptr04x(vhci), ptr04x(dev));

                WdfRequestComplete(request, STATUS_NOT_SUPPORTED);
                return;
        }

        TraceDbg("dev %04x", ptr04x(dev));

        auto st = device::reset_port(dev, request);
        if (st != STATUS_PENDING) {
                Trace(TRACE_LEVEL_ERROR, "dev %04x, reset port %!STATUS!", ptr04x(dev), st);
                WdfRequestComplete(request, st);
        }
}

_Function_class_(EVT_WDF_OBJECT_CONTEXT_CLEANUP)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
PAGED void NTAPI endpoint_cleanup(_In_ WDFOBJECT object)
{
        PAGED_CODE();

        auto endpoint = static_cast<UDECXUSBENDPOINT>(object);
        auto &endp = *get_endpoint_ctx(endpoint);
        auto &d = endp.descriptor;

        TraceDbg("endp %04x{Address %#x: %s %s[%d]}, PipeHandle %04x",
                  ptr04x(endpoint), d.bEndpointAddress, usbd_pipe_type_str(usb_endpoint_type(d)),
                  usb_endpoint_dir_out(d) ? "Out" : "In", usb_endpoint_num(d), ptr04x(endp.PipeHandle));

        remove_endpoint_list(endp);
}

/*
 * FIXME: UDE never(?) call this callback for stalled endpoints.
 */
_Function_class_(EVT_UDECX_USB_ENDPOINT_RESET)
_IRQL_requires_same_
void endpoint_reset(_In_ UDECXUSBENDPOINT endp, _In_ WDFREQUEST request)
{
        NT_ASSERT(!has_urb(request));
        TraceDbg("endp %04x, req %04x", ptr04x(endp), ptr04x(request));

        auto st = device::clear_endpoint_stall(endp, request);
        if (st != STATUS_PENDING) {
                Trace(TRACE_LEVEL_ERROR, "endp %04x, %!STATUS!", ptr04x(endp), st);
                WdfRequestComplete(request, st);
        }
}

_Function_class_(EVT_UDECX_USB_ENDPOINT_PURGE)
_IRQL_requires_same_
void endpoint_purge(_In_ UDECXUSBENDPOINT endpoint)
{
        auto &endp = *get_endpoint_ctx(endpoint);
        auto &dev = *get_device_ctx(endp.device);

        TraceDbg("dev %04x, endp %04x, queue %04x", ptr04x(endp.device), ptr04x(endpoint), ptr04x(endp.queue));

        while (auto request = device::remove_request(dev, endpoint)) {
                device::send_cmd_unlink_and_cancel(endp.device, request);
        }

        auto purge_complete = [] ([[maybe_unused]] auto queue, auto ctx) // EVT_WDF_IO_QUEUE_STATE
        { 
                auto endpoint = static_cast<UDECXUSBENDPOINT>(ctx);
                NT_ASSERT(get_endpoint(queue) == endpoint);

                UdecxUsbEndpointPurgeComplete(endpoint);
        };

        WdfIoQueuePurge(endp.queue, purge_complete, endpoint);
}

_Function_class_(EVT_UDECX_USB_ENDPOINT_START)
_IRQL_requires_same_
void endpoint_start(_In_ UDECXUSBENDPOINT endp)
{
        auto queue = get_endpoint_ctx(endp)->queue;
        TraceDbg("endp %04x, queue %04x", ptr04x(endp), ptr04x(queue));
        WdfIoQueueStart(queue);
}

/*
 * Bring the device from low power state and enter working state.
 * The power request may be completed asynchronously by returning STATUS_PENDING, 
 * and then later completing it by calling UdecxUsbDeviceLinkPowerExitComplete 
 * with the actual completion code.
 */
_Function_class_(EVT_UDECX_USB_DEVICE_D0_ENTRY)
_IRQL_requires_same_
NTSTATUS d0_entry(_In_ WDFDEVICE vhci, _In_ UDECXUSBDEVICE dev)
{
        TraceDbg("vhci %04x, dev %04x", ptr04x(vhci), ptr04x(dev));
        return STATUS_SUCCESS;
}

/*
 * Send the virtual USB device to a low power state.
 * The power request may be completed asynchronously by returning STATUS_PENDING, 
 * and then later calling UdecxUsbDeviceLinkPowerExitComplete with the actual completion code.
 * 
 * @see UdecxUsbDeviceSignalWake, UdecxUsbDeviceSignalFunctionWake
 */
_Function_class_(EVT_UDECX_USB_DEVICE_D0_EXIT)
_IRQL_requires_same_
NTSTATUS d0_exit(_In_ WDFDEVICE vhci, _In_ UDECXUSBDEVICE dev, _In_ UDECX_USB_DEVICE_WAKE_SETTING WakeSetting)
{
        TraceDbg("vhci %04x, dev %04x, %!UDECX_USB_DEVICE_WAKE_SETTING!", ptr04x(vhci), ptr04x(dev), WakeSetting);
        
        switch (WakeSetting) {
        case UdecxUsbDeviceWakeDisabled:
                break;
        case UdecxUsbDeviceWakeEnabled:
                NT_ASSERT(get_device_ctx(dev)->speed() < USB_SPEED_SUPER);
                break;
        case UdecxUsbDeviceWakeNotApplicable: // SuperSpeed device
                NT_ASSERT(get_device_ctx(dev)->speed() >= USB_SPEED_SUPER);
                break;
        }

        return STATUS_SUCCESS;
}

/*
 * @see UdecxUsbDeviceSignalFunctionWake
 * @see UdecxUsbDeviceSetFunctionSuspendAndWakeComplete
 */
_Function_class_(EVT_UDECX_USB_DEVICE_SET_FUNCTION_SUSPEND_AND_WAKE)
_IRQL_requires_same_
NTSTATUS function_suspend_and_wake(
        _In_ WDFDEVICE vhci, 
        _In_ UDECXUSBDEVICE dev, 
        _In_ ULONG Interface, 
        _In_ UDECX_USB_DEVICE_FUNCTION_POWER FunctionPower)
{
        TraceDbg("vhci %04x, dev %04x, Interface %lu, %!UDECX_USB_DEVICE_FUNCTION_POWER!", 
                  ptr04x(vhci), ptr04x(dev), Interface, FunctionPower);

        NT_ASSERT(get_device_ctx(dev)->speed() >= USB_SPEED_SUPER);

        switch (FunctionPower) {
        case UdecxUsbDeviceFunctionNotSuspended:
                break;
        case UdecxUsbDeviceFunctionSuspendedCannotWake:
                break;
        case UdecxUsbDeviceFunctionSuspendedCanWake:
                break;
        }

        return STATUS_SUCCESS;
}

/*
 * UDE can call EvtIoInternalDeviceControl concurrently for different queues of the same device.
 * I've caught concurrent CTRL and BULK transfer for a flash drive. 
 * FIXME: can UDE reorder requests?
 *
 * WdfSynchronizationScopeDevice can't be used to serialize calls of WskSend because 
 * it can be called concurrently from UDECX_USB_ENDPOINT_CALLBACKS.EvtUsbEndpointPurge.
 * If set SynchronizationScopeDevice for UDECXUSBENDPOINT, UdecxUsbEndpointCreate 
 * will return STATUS_WDF_SYNCHRONIZATION_SCOPE_INVALID. For these reasons,
 * explicit WDFSPINLOCK device_ctx.send_lock is used to serialize WskSend calls.
 * 
 * Using power-managed queues for I/O requests that require the device to be in its working state, 
 * and using queues that are not power-managed for all other requests.
 * Virtual device does not require power. For that reason all queues are not power-managed. 
 * @see Power Management for I/O Queues
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto create_endpoint_queue(_Inout_ WDFQUEUE &queue, _In_ UDECXUSBENDPOINT endpoint)
{
        PAGED_CODE();
        
        WDF_IO_QUEUE_CONFIG cfg;
        WDF_IO_QUEUE_CONFIG_INIT(&cfg, WdfIoQueueDispatchParallel); // FIXME: Sequential for EP0?
        cfg.PowerManaged = WdfFalse;
        cfg.EvtIoInternalDeviceControl = device::internal_control;

        WDF_OBJECT_ATTRIBUTES attr;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, UDECXUSBENDPOINT);
        attr.EvtCleanupCallback = [] (auto obj) { TraceDbg("Queue %04x cleanup", ptr04x(obj)); };
//      attr.SynchronizationScope = WdfSynchronizationScopeQueue; // EvtIoInternalDeviceControl is used only
        attr.ParentObject = endpoint;

        auto &endp = *get_endpoint_ctx(endpoint);
        auto &dev = *get_device_ctx(endp.device); 

        if (auto err = WdfIoQueueCreate(dev.vhci, &cfg, &attr, &queue)) {
                Trace(TRACE_LEVEL_ERROR, "WdfIoQueueCreate %!STATUS!", err);
                return err;
        }
        get_endpoint(queue) = endpoint;

        UdecxUsbEndpointSetWdfIoQueue(endpoint, queue);
        return STATUS_SUCCESS;
}

/*
 * UDE can call UDECX_USB_DEVICE_STATE_CHANGE_CALLBACKS despite UdecxUsbDevicePlugOutAndDelete was called.
 * This can cause BSOD:
 * WDF_VIOLATION
 * A NULL parameter was passed to a function that required a non-NULL value
 * udecx!Endpoint_UcxEndpointCleanup
 * 
 * To fix it, callbacks and UdecxUsbDevicePlugOutAndDelete must be called serially.
 * WdfObjectAcquireLock for UDECXUSBDEVICE can't be used, it will use spinlock that raises IRQL.
 */
_Function_class_(EVT_UDECX_USB_DEVICE_ENDPOINT_ADD)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS endpoint_add(_In_ UDECXUSBDEVICE device, _In_ UDECX_USB_ENDPOINT_INIT_AND_METADATA *data)
{
        PAGED_CODE();
        
        auto &dev = *get_device_ctx(device);
        if (dev.unplugged) {
                TraceDbg("dev %04x, unplugged", ptr04x(device));
                return STATUS_DEVICE_NOT_CONNECTED;
        }

        wdf::WaitLock lck(dev.delete_lock);

        auto &epd = data->EndpointDescriptor ? *data->EndpointDescriptor : EP0;
        UdecxUsbEndpointInitSetEndpointAddress(data->UdecxUsbEndpointInit, epd.bEndpointAddress);

        UDECX_USB_ENDPOINT_CALLBACKS cb;
        UDECX_USB_ENDPOINT_CALLBACKS_INIT(&cb, endpoint_reset);
        cb.EvtUsbEndpointStart = endpoint_start; // does not work without it
        cb.EvtUsbEndpointPurge = endpoint_purge;

        UdecxUsbEndpointInitSetCallbacks(data->UdecxUsbEndpointInit, &cb);

        WDF_OBJECT_ATTRIBUTES attr;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, endpoint_ctx);
        attr.EvtCleanupCallback = endpoint_cleanup;
        attr.ParentObject = device;

        UDECXUSBENDPOINT endpoint;
        if (auto err = UdecxUsbEndpointCreate(&data->UdecxUsbEndpointInit, &attr, &endpoint)) {
                Trace(TRACE_LEVEL_ERROR, "UdecxUsbEndpointCreate %!STATUS!", err);
                return err;
        }

        auto &endp = *get_endpoint_ctx(endpoint);

        endp.device = device;
        InitializeListHead(&endp.entry);

        if (auto len = data->EndpointDescriptorBufferLength) {
                NT_ASSERT(epd.bLength == len);
                NT_ASSERT(sizeof(endp.descriptor) >= len);
                RtlCopyMemory(&endp.descriptor, &epd, len);
                insert_endpoint_list(endp);
        } else {
                NT_ASSERT(epd == EP0);
                static_cast<USB_ENDPOINT_DESCRIPTOR&>(endp.descriptor) = epd;
                dev.ep0 = endpoint;
        }

        if (auto err = create_endpoint_queue(endp.queue, endpoint)) {
                return err;
        }

        {
                auto &d = endp.descriptor;
                TraceDbg("dev %04x, endp %04x{Length %d, Address %#04x{%s %s[%d]}, Attributes %#x, MaxPacketSize %#x, "
                         "Interval %d, Audio{Refresh %#x, SynchAddress %#x}}, queue %04x%!BIN!",
                        ptr04x(device), ptr04x(endpoint), d.bLength, d.bEndpointAddress, 
                        usbd_pipe_type_str(usb_endpoint_type(d)), usb_endpoint_dir_out(d) ? "Out" : "In", 
                        usb_endpoint_num(d), d.bmAttributes, d.wMaxPacketSize, d.bInterval, d.bRefresh,
                        d.bSynchAddress, ptr04x(endp.queue), WppBinary(&d, d.bLength));
        }

        return STATUS_SUCCESS;
}

_Function_class_(EVT_UDECX_USB_DEVICE_DEFAULT_ENDPOINT_ADD)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS default_endpoint_add(_In_ UDECXUSBDEVICE dev, _In_ _UDECXUSBENDPOINT_INIT *init)
{
        PAGED_CODE();
        UDECX_USB_ENDPOINT_INIT_AND_METADATA data{ .UdecxUsbEndpointInit = init };
        return endpoint_add(dev, &data);
}

/*
 * WdfRequestGetIoQueue(request) returns queue that does not belong to the device (not its EP0 or others).
 * get_endpoint(WdfRequestGetIoQueue(request)) causes BSOD.
 *
 * Completing a request with an error status after UdecxUsbDevicePlugOutAndDelete causes 
 * hanging of the driver during the unloading because UDE will not destroy UDECXUSBDEVICE.
 *
 * UDEX does not call ConfigurationChange for composite devices.
 * UDEX passes wrong InterfaceNumber, NewInterfaceSetting for InterfaceSettingChange.
 * For that reasons the Upper Filter Driver is used that intercepts SELECT_CONFIGURATION, 
 * SELECT_INTERFACE URBs and notifies this driver.
 *
 * @see ude_filter/int_dev_ctrl.cpp, int_dev_ctrl
 * @see device_ioctl.cpp, control_transfer
 */
_Function_class_(EVT_UDECX_USB_DEVICE_ENDPOINTS_CONFIGURE)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void endpoints_configure(
        _In_ UDECXUSBDEVICE device, _In_ WDFREQUEST request, _In_ UDECX_ENDPOINTS_CONFIGURE_PARAMS *params)
{
        NT_ASSERT(!has_urb(request)); // but MajorFunction == IRP_MJ_INTERNAL_DEVICE_CONTROL

//      wdf::WaitLock lck(dev.delete_lock); // not required, only logging there

        if (auto n = params->EndpointsToConfigureCount) {
                TraceDbg("dev %04x, EndpointsToConfigure[%lu]%!BIN!", ptr04x(device), n, 
                          WppBinary(params->EndpointsToConfigure, USHORT(n*sizeof(*params->EndpointsToConfigure))));
        }

        if (auto n = params->ReleasedEndpointsCount) {
                TraceDbg("dev %04x, ReleasedEndpoints[%lu]%!BIN!", ptr04x(device), n, 
                          WppBinary(params->ReleasedEndpoints, USHORT(n*sizeof(*params->ReleasedEndpoints))));
        }

        switch (params->ConfigureType) {
        case UdecxEndpointsConfigureTypeDeviceInitialize: // for internal use, can be called several times
                TraceDbg("dev %04x, DeviceInitialize", ptr04x(device));
                break;
        case UdecxEndpointsConfigureTypeDeviceConfigurationChange:
                TraceDbg("dev %04x, ConfigurationValue %d", ptr04x(device), params->NewConfigurationValue);
                break;
        case UdecxEndpointsConfigureTypeInterfaceSettingChange:
                TraceDbg("dev %04x, InterfaceNumber %d, NewInterfaceSetting %d", 
                          ptr04x(device), params->InterfaceNumber, params->NewInterfaceSetting);
                break;
        case UdecxEndpointsConfigureTypeEndpointsReleasedOnly:
                TraceDbg("dev %04x, EndpointsReleasedOnly", ptr04x(device)); // WdfObjectDelete(ReleasedEndpoints[i]) can cause BSOD
                break;
        }

        WdfRequestComplete(request, STATUS_SUCCESS);
}

/*
 * _UDECXUSBDEVICE_INIT* must be freed if UdecxUsbDeviceCreate fails.
 * UdecxUsbDeviceInitFree must not be called on success, Udecx does that itself.
 */
struct device_init_ptr
{
        device_init_ptr(WDFDEVICE vhci) : ptr(UdecxUsbDeviceInitAllocate(vhci)) {}

        ~device_init_ptr() 
        {
                if (ptr) {
                        UdecxUsbDeviceInitFree(ptr);
                }
        }

        device_init_ptr(const device_init_ptr&) = delete;
        device_init_ptr& operator=(const device_init_ptr&) = delete;

        explicit operator bool() const { return ptr; }
        auto operator !() const { return !ptr; }

        _UDECXUSBDEVICE_INIT *ptr{};
};

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto prepare_init(_In_ _UDECXUSBDEVICE_INIT *init, _In_ device_ctx_ext &ext)
{
        PAGED_CODE();
        NT_ASSERT(init);

        UDECX_USB_DEVICE_STATE_CHANGE_CALLBACKS cb;
        UDECX_USB_DEVICE_CALLBACKS_INIT(&cb);

        cb.EvtUsbDeviceLinkPowerEntry = d0_entry; // required if the device supports USB remote wake
        cb.EvtUsbDeviceLinkPowerExit = d0_exit;
        
        if (ext.dev.speed >= USB_SPEED_SUPER) { // required
                cb.EvtUsbDeviceSetFunctionSuspendAndWake = function_suspend_and_wake;
        }

//      cb.EvtUsbDeviceReset = device_reset; // server returns EPIPE always
        
        cb.EvtUsbDeviceDefaultEndpointAdd = default_endpoint_add;
        cb.EvtUsbDeviceEndpointAdd = endpoint_add;
        cb.EvtUsbDeviceEndpointsConfigure = endpoints_configure;

        UdecxUsbDeviceInitSetStateChangeCallbacks(init, &cb);

        auto speed = to_udex_speed(ext.dev.speed);
        TraceDbg("%!usb_device_speed! -> %!UDECX_USB_DEVICE_SPEED!", ext.dev.speed, speed);

        UdecxUsbDeviceInitSetSpeed(init, speed);
        UdecxUsbDeviceInitSetEndpointsType(init, UdecxEndpointTypeDynamic);

        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto create_delete_lock(_Out_ WDFWAITLOCK &handle, _In_ WDFOBJECT parent)
{
        PAGED_CODE();

        WDF_OBJECT_ATTRIBUTES attr;
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = parent;

        if (auto err = WdfWaitLockCreate(&attr, &handle)) {
                Trace(TRACE_LEVEL_ERROR, "WdfWaitLockCreate %!STATUS!", err);
                return err;
        }

        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto create_spin_lock(_Out_ WDFSPINLOCK *handle, _In_ WDFOBJECT parent)
{
        PAGED_CODE();

        WDF_OBJECT_ATTRIBUTES attr;
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = parent;

        if (auto err = WdfSpinLockCreate(&attr, handle)) {
                Trace(TRACE_LEVEL_ERROR, "WdfSpinLockCreate %!STATUS!", err);
                return err;
        }

        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto init_device(_In_ UDECXUSBDEVICE device, _Inout_ device_ctx &dev)
{
        PAGED_CODE();

        WDFSPINLOCK *v[] = {
                &dev.send_lock,
                &dev.endpoint_list_lock,
                &dev.requests_lock,
        };

        for (auto i: v) {
                if (auto err = create_spin_lock(i, device)) {
                        return err;
                }
        }

        if (auto err = create_delete_lock(dev.delete_lock, device)) {
                return err;
        }

        InitializeListHead(&dev.requests);
        KeInitializeEvent(&dev.detach_completed, NotificationEvent, false);

        return STATUS_SUCCESS;
}

inline auto set_unplugged(_Inout_ device_ctx &dev)
{
        static_assert(sizeof(dev.unplugged) == sizeof(CHAR));
        return InterlockedExchange8(PCHAR(&dev.unplugged), true);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto plugout_and_delete(_In_ UDECXUSBDEVICE dev, _In_ WDFWAITLOCK delete_lock)
{
        PAGED_CODE();

        wdf::WaitLock lck(delete_lock);
        if (auto err = UdecxUsbDevicePlugOutAndDelete(dev)) { // caught BSOD on DISPATCH_LEVEL
                Trace(TRACE_LEVEL_ERROR, "dev %04x, UdecxUsbDevicePlugOutAndDelete %!STATUS!", ptr04x(dev), err);
                return false;
        }
        return true;
}

/*
 * Call UdecxUsbDevicePlugOutAndDelete if UdecxUsbDevicePlugIn was successful.
 * After UdecxUsbDevicePlugOutAndDelete the client driver can no longer use UDECXUSBDEVICE.
 * 
 * FIXME:
 * If call UdecxUsbDevicePlugOutAndDelete shortly after UdecxUsbDevicePlugIn,
 * it is highly likely it will not delete UDECXUSBDEVICE immediately.
 * In such case UDECXUSBDEVICE.EvtCleanupCallback will be called during the deletion of VHCI device only.
 * The reason is unknown, there are no any clarifications in API documentation about that.
 * 
 * Therefore we have to close a socket and free a port here instead of device_cleanup, 
 * otherwise next plugin_hardware will fail with ST_DEV_BUSY because the socket is still connected.
 *
 * FIXME: inability to delete UDECXUSBDEVICE causes leak of resources, drivers' memory consumption 
 * raises over the time. Driver Verifier cannot detect it because all will be freed on driver unload.
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto detach(_In_ UDECXUSBDEVICE device)
{
        PAGED_CODE();

        auto &dev = *get_device_ctx(device);
	NT_ASSERT(dev.unplugged);

        if (close_socket(dev.sock())) {
                Trace(TRACE_LEVEL_INFORMATION, "dev %04x, connection closed", ptr04x(device));
        }

        auto thread = recv_thread_join(device, dev);

        auto port = vhci::reclaim_roothub_port(device);
        if (port) {
                Trace(TRACE_LEVEL_INFORMATION, "port %d released", port);
        }

        if (plugout_and_delete(device, dev.delete_lock)) {
                Trace(TRACE_LEVEL_INFORMATION, "dev %04x, PlugOutAndDelete-d", ptr04x(device));
        }

        NT_VERIFY(!KeSetEvent(&dev.detach_completed, IO_NO_INCREMENT, false)); // once
        return thread;
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
auto create_workitem(_Out_ WDFWORKITEM &wi, _In_ UDECXUSBDEVICE device)
{
        auto func = [] (auto WorkItem)
        {
                if (auto dev = (UDECXUSBDEVICE)WdfWorkItemGetParentObject(WorkItem)) {
                        NT_VERIFY(!detach(dev));
                }
                WdfObjectDelete(WorkItem);
        };

        WDF_WORKITEM_CONFIG cfg;
        WDF_WORKITEM_CONFIG_INIT(&cfg, func);
        cfg.AutomaticSerialization = false;

        WDF_OBJECT_ATTRIBUTES attr;
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = device;

        auto st = WdfWorkItemCreate(&cfg, &attr, &wi);
        if (NT_ERROR(st)) {
                Trace(TRACE_LEVEL_ERROR, "dev %04x, WdfWorkItemCreate %!STATUS!", ptr04x(device), st);
                wi = WDF_NO_HANDLE;
        }

        return st;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto wait_detach(_In_ UDECXUSBDEVICE device, _In_opt_ LARGE_INTEGER *timeout = nullptr)
{
        PAGED_CODE();
        TraceDbg("dev %04x", ptr04x(device));

        auto &dev = *get_device_ctx(device);
        NT_ASSERT(dev.unplugged);

        auto st = KeWaitForSingleObject(&dev.detach_completed, Executive, KernelMode, false, timeout);

        switch (st) {
        case STATUS_SUCCESS:
                TraceDbg("dev %04x, completed", ptr04x(device));
                break;
        case STATUS_TIMEOUT: // a bug in the driver
                TraceDbg("dev %04x, timeout (purged WDFREQUEST is not completed?)", ptr04x(device));
                static_assert(NT_SUCCESS(STATUS_TIMEOUT));
                st = STATUS_OPERATION_IN_PROGRESS;
                break;
        default:
                Trace(TRACE_LEVEL_ERROR, "dev %04x, KeWaitForSingleObject %!STATUS!", ptr04x(device), st);
        }

        return st;
}

constexpr auto wait_detach_timeout()
{
        return make_timeout(30*wdm::second, wdm::period::relative);
}

} // namespace


_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::device::create(_Out_ UDECXUSBDEVICE &device, _In_ WDFDEVICE vhci, _In_ device_ctx_ext *ext)
{
        PAGED_CODE();
        device = WDF_NO_HANDLE;

        device_init_ptr init(vhci);
        if (auto err = init ? prepare_init(init.ptr, *ext) : STATUS_INSUFFICIENT_RESOURCES) {
                Trace(TRACE_LEVEL_ERROR, "UDECXUSBDEVICE_INIT %!STATUS!", err);
                return err;
        }

        WDF_OBJECT_ATTRIBUTES attr;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, device_ctx);
        attr.EvtCleanupCallback = device_cleanup;
        attr.EvtDestroyCallback = device_destroy;
        attr.ParentObject = vhci; // FIXME: by default?

        if (auto err = UdecxUsbDeviceCreate(&init.ptr, &attr, &device)) {
                Trace(TRACE_LEVEL_ERROR, "UdecxUsbDeviceCreate %!STATUS!", err);
                return err;
        }

        NT_ASSERT(!init); // zeroed by UdecxUsbDeviceCreate
        auto &ctx = *get_device_ctx(device);

        ctx.vhci = vhci;
        ctx.ext = ext;
        ext->ctx = &ctx;

        if (auto err = init_device(device, ctx)) {
                return err;
        }

        Trace(TRACE_LEVEL_INFORMATION, "dev %04x", ptr04x(device));
        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::device::recv_thread_start(_In_ UDECXUSBDEVICE device)
{
        PAGED_CODE();
        const auto access = THREAD_ALL_ACCESS;

        HANDLE handle{};
        if (auto err = PsCreateSystemThread(&handle, access, nullptr, nullptr, nullptr, recv_thread_function, device)) {
                Trace(TRACE_LEVEL_ERROR, "PsCreateSystemThread %!STATUS!", err);
                return err;
        }

        auto dev = get_device_ctx(device);

        NT_VERIFY(NT_SUCCESS(ObReferenceObjectByHandle(handle, access, *PsThreadType, KernelMode, 
                                                       reinterpret_cast<PVOID*>(&dev->recv_thread), nullptr)));

        NT_VERIFY(NT_SUCCESS(ZwClose(handle)));

        TraceDbg("dev %04x", ptr04x(device));
        return STATUS_SUCCESS;
}

/*
 * WdfIoQueuePurge(,PurgeComplete,) could be used instead of WdfWorkItem if set queue's ExecutionLevel
 * to WdfExecutionLevelPassive. But in this case WDF constantly use worker thread on DPC level:
 *
 * FxIoQueue::DispatchEvents:Thread FFFFB608248E1040 is processing WDFQUEUE 0x000049F7C9CD2758
 * TRACE_LEVEL_WARNING FxIoQueue::CanThreadDispatchEventsLocked:Current thread 0xFFFFB608197AA040 is not
 *      at the passive-level 0x00000002(DPC), posting to worker thread for WDFQUEUE 0x000049F7C9CD2758
 * TRACE_LEVEL_VERBOSE FxIoQueue::_DeferredDispatchThreadThunk:Dispatching requests from worker thread
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS usbip::device::async_detach_nowait(_In_ UDECXUSBDEVICE device)
{
        static_assert(NT_SUCCESS(STATUS_PENDING));

        auto &dev = *get_device_ctx(device);
        if (dev.unplugged) {
                TraceDbg("dev %04x, already unplugged", ptr04x(device));
                return STATUS_PENDING;
        }
        
        WDFWORKITEM wi{};
        if (auto err = create_workitem(wi, device)) {
                NT_ASSERT(!wi);
                return err;
        }

        if (auto was_unplugged = set_unplugged(dev)) { // double-checked pattern
                TraceDbg("dev %04x, already unplugged", ptr04x(device));
                WdfObjectDelete(wi);
                return STATUS_PENDING;
        }

        WdfWorkItemEnqueue(wi);
        return STATUS_SUCCESS;
}

/*
 * Do not call WdfIoQueuePurgeSynchronously from the following queue object event callback functions,
 * regardless of the queue with which the event callback function is associated:
 * EvtIoDefault, EvtIoDeviceControl, EvtIoInternalDeviceControl, EvtIoRead, EvtIoWrite.
 *
 * Must wait for the completion because plugin_hardware can be called prior ::detach().
 * In such case plugin_hardware will fail with USBIP_ERROR_ST_DEV_BUSY because the socket is still connected.
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::device::async_detach_and_wait(_In_ UDECXUSBDEVICE device)
{
        PAGED_CODE();

        auto st = async_detach_nowait(device);
        if (NT_SUCCESS(st)) {
                auto timeout = wait_detach_timeout();
                st = wait_detach(device, &timeout);
        }
        return st;
}

/*
 * @see plugout_and_delete
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::device::detach(_In_ UDECXUSBDEVICE device)
{
        PAGED_CODE();
        auto &dev = *get_device_ctx(device);

        if (auto was_unplugged = set_unplugged(dev)) {
                TraceDbg("dev %04x, already unplugged", ptr04x(device));
        } else if (auto thread = ::detach(device)) {
                NT_ASSERT(thread == KeGetCurrentThread());
                ObDereferenceObjectDeferDelete(thread);
                return STATUS_SUCCESS;
        }

        auto timeout = wait_detach_timeout();
        return wait_detach(device, &timeout); // concurrent calls wait for the completion
}
