/*
 * Copyright (C) 2022 - 2023 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "driver.h"
#include "vhci.h"
#include "trace.h"
#include "driver.tmh"

#include "context.h"
#include "wsk_context.h"

#include <libdrv\wsk_cpp.h>

namespace
{

using namespace usbip;

_Function_class_(EVT_WDF_OBJECT_CONTEXT_CLEANUP)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
PAGED void driver_cleanup(_In_ WDFOBJECT Object)
{
	PAGED_CODE();

	auto drv = static_cast<WDFDRIVER>(Object);
	Trace(TRACE_LEVEL_INFORMATION, "%04x", ptr04x(drv));

	//wsk::shutdown();
	//delete_wsk_context_list();

	auto drvobj = WdfDriverWdmGetDriverObject(drv);
	WPP_CLEANUP(drvobj);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
CS_INIT auto driver_create(_In_ DRIVER_OBJECT *DriverObject, _In_ UNICODE_STRING *RegistryPath)
{
	PAGED_CODE();

	WDF_OBJECT_ATTRIBUTES attr;
	WDF_OBJECT_ATTRIBUTES_INIT(&attr);
	attr.EvtCleanupCallback = driver_cleanup;

	WDF_DRIVER_CONFIG cfg;
	WDF_DRIVER_CONFIG_INIT(&cfg, DeviceAdd);
	cfg.DriverPoolTag = pooltag;

	return WdfDriverCreate(DriverObject, RegistryPath, &attr, &cfg, nullptr);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
CS_INIT auto init()
{
	PAGED_CODE();
	return STATUS_SUCCESS;
}

} // namespace


_Function_class_(DRIVER_INITIALIZE)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
CS_INIT EXTERN_C NTSTATUS DriverEntry(_In_ DRIVER_OBJECT *DriverObject, _In_ UNICODE_STRING *RegistryPath)
{
	ExInitializeDriverRuntime(0); // @see ExAllocatePool2

	if (auto err = driver_create(DriverObject, RegistryPath)) {
		return err;
	}

	WPP_INIT_TRACING(DriverObject, RegistryPath);
	Trace(TRACE_LEVEL_INFORMATION, "RegistryPath '%!USTR!'", RegistryPath);

	return init();
}
