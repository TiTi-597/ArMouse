#include <ntifs.h>
#include "ArMouse.hpp"

__declspec(noinline) VOID DriverExit(PDRIVER_OBJECT DriverObject) {
	UNREFERENCED_PARAMETER(DriverObject);
	CleanupMouseHook();
}

__declspec(noinline) NTSTATUS DriverInit(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegisteryPath) {
	UNREFERENCED_PARAMETER(RegisteryPath);
	DriverObject->DriverUnload = DriverExit;

	InitializeMouseHook();
	QueueMouseDelta(100,100);

	return STATUS_SUCCESS;
}