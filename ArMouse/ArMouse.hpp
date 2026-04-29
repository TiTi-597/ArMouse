typedef struct _MOUSE_INPUT_DATA {
	USHORT UnitId;
	USHORT Flags;
	union {
		ULONG Buttons;
		struct {
			USHORT  ButtonFlags;
			USHORT  ButtonData;
		};
	};
	ULONG RawButtons;
	LONG LastX;
	LONG LastY;
	ULONG ExtraInformation;
} MOUSE_INPUT_DATA, * PMOUSE_INPUT_DATA;

typedef VOID(*_MouseClassServiceCallback)(PDEVICE_OBJECT, PMOUSE_INPUT_DATA, PMOUSE_INPUT_DATA, PULONG);

typedef struct _MOUSE_HOOK_CONTEXT {
	KSPIN_LOCK PendingDeltaSpinLock;
	LONG PendingDeltaX;
	LONG PendingDeltaY;
	PVOID OriginalMouHidCallback;
	BOOLEAN HookInstalled;
	BOOLEAN Initialized;
} MOUSE_HOOK_CONTEXT, * PMOUSE_HOOK_CONTEXT;

EXTERN_C POBJECT_TYPE* IoDriverObjectType;
EXTERN_C NTSTATUS ObReferenceObjectByName(PUNICODE_STRING ObjectName, ULONG Attributes, PACCESS_STATE AccessState, ACCESS_MASK DesiredAccess, POBJECT_TYPE ObjectType, KPROCESSOR_MODE AccessMode, PVOID ParseContext, PVOID* Object);

MOUSE_HOOK_CONTEXT MouseHookCtx;

__forceinline KIRQL RaiseIrql(KIRQL NewIrql) {
	KIRQL result = (KIRQL)__readcr8();
	__writecr8(NewIrql);
	return result;
}

__forceinline VOID LowerIrql(KIRQL NewIrql) {
	__writecr8(NewIrql);
}

__forceinline KIRQL AcquireSpinLock(PKSPIN_LOCK spinLock) {
	KIRQL oldIrql = RaiseIrql(DISPATCH_LEVEL);
	while (_InterlockedCompareExchangePointer((PVOID volatile*)spinLock, (PVOID)1, (PVOID)0) != 0) {
		_mm_pause();
	}
	return oldIrql;
}

__forceinline VOID ReleaseSpinLock(PKSPIN_LOCK spinLock) {
	_InterlockedExchangePointer((PVOID volatile*)spinLock, (PVOID)0);
}

__declspec(noinline) VOID MouHidHookCallback(PDEVICE_OBJECT DeviceObject, PMOUSE_INPUT_DATA InputStart, PMOUSE_INPUT_DATA InputEnd, PULONG InputConsumed) {
	LONG dx = 0;
	LONG dy = 0;

	KIRQL oldIrql = AcquireSpinLock(&MouseHookCtx.PendingDeltaSpinLock);
	if (MouseHookCtx.PendingDeltaX || MouseHookCtx.PendingDeltaY) {
		dx = MouseHookCtx.PendingDeltaX;
		dy = MouseHookCtx.PendingDeltaY;
		MouseHookCtx.PendingDeltaX = 0;
		MouseHookCtx.PendingDeltaY = 0;
	}
	ReleaseSpinLock(&MouseHookCtx.PendingDeltaSpinLock);
	LowerIrql(oldIrql);

	if ((dx || dy) && InputStart && InputStart < InputEnd) {
		for (PMOUSE_INPUT_DATA cur = InputStart; cur < InputEnd; ++cur) {
			cur->LastX += dx;
			cur->LastY += dy;
		}
	}

	if (MouseHookCtx.OriginalMouHidCallback) ((_MouseClassServiceCallback)MouseHookCtx.OriginalMouHidCallback)(DeviceObject, InputStart, InputEnd, InputConsumed);
}

__declspec(noinline) NTSTATUS HookMouHidCallback() {
	if (MouseHookCtx.HookInstalled) return STATUS_SUCCESS;

	UNICODE_STRING driverName = { 0 };
	PDRIVER_OBJECT pMouClass = NULL;
	PDRIVER_OBJECT pMouHid = NULL;

	RtlInitUnicodeString(&driverName, L"\\Driver\\MouClass");
	if (!IoDriverObjectType || !*IoDriverObjectType) return STATUS_OBJECT_TYPE_MISMATCH;

	NTSTATUS status = ObReferenceObjectByName(&driverName, OBJ_CASE_INSENSITIVE, NULL, 0, *IoDriverObjectType, KernelMode, NULL, (PVOID*)&pMouClass);
	if (!NT_SUCCESS(status)) return status;

	RtlInitUnicodeString(&driverName, L"\\Driver\\MouHID");
	status = ObReferenceObjectByName(&driverName, OBJ_CASE_INSENSITIVE, NULL, 0, *IoDriverObjectType, KernelMode, NULL, (PVOID*)&pMouHid);
	if (!NT_SUCCESS(status)) {
		ObfDereferenceObject(pMouClass);
		return status;
	}

	ULONG64 mouClassBase = (ULONG64)pMouClass->DriverStart;
	ULONG32 mouClassSize = pMouClass->DriverSize;
	BOOLEAN hooked = FALSE;

	for (PDEVICE_OBJECT deviceObj = pMouHid->DeviceObject; deviceObj; deviceObj = deviceObj->AttachedDevice) {
		PUCHAR ext = (PUCHAR)deviceObj->DeviceExtension;
		if (!ext) continue;

		for (SIZE_T off = 0; off + sizeof(PVOID) <= 0x1FF8; off += sizeof(PVOID)) {
			PVOID* slot = (PVOID*)(ext + off);
			if (!MmIsAddressValid(slot)) continue;

			PVOID value = *slot;
			if (!value || !MmIsAddressValid(value)) continue;
			if (value == (PVOID)MouHidHookCallback) continue;

			ULONG64 address = (ULONG64)value;
			if (address >= mouClassBase && address < mouClassBase + mouClassSize) {
				if (!hooked) {
					MouseHookCtx.OriginalMouHidCallback = value;
				}
				*slot = (PVOID)MouHidHookCallback;
				hooked = TRUE;
			}
		}
	}

	if (hooked) MouseHookCtx.HookInstalled = TRUE;

	ObfDereferenceObject(pMouHid);
	ObfDereferenceObject(pMouClass);

	return hooked ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

__declspec(noinline) NTSTATUS InitializeMouseHook() {
	if (MouseHookCtx.Initialized && MouseHookCtx.HookInstalled) return STATUS_SUCCESS;

	MouseHookCtx.PendingDeltaSpinLock = 0;
	MouseHookCtx.PendingDeltaX = 0;
	MouseHookCtx.PendingDeltaY = 0;
	MouseHookCtx.OriginalMouHidCallback = NULL;
	MouseHookCtx.HookInstalled = FALSE;

	NTSTATUS status = HookMouHidCallback();
	if (!NT_SUCCESS(status)) return status;

	MouseHookCtx.Initialized = TRUE;
	return STATUS_SUCCESS;
}

__declspec(noinline) BOOLEAN QueueMouseDelta(LONG dx, LONG dy) {
	if (!MouseHookCtx.Initialized || !MouseHookCtx.HookInstalled) return FALSE;

	KIRQL oldIrql = AcquireSpinLock(&MouseHookCtx.PendingDeltaSpinLock);
	MouseHookCtx.PendingDeltaX += dx;
	MouseHookCtx.PendingDeltaY += dy;
	ReleaseSpinLock(&MouseHookCtx.PendingDeltaSpinLock);
	LowerIrql(oldIrql);

	return TRUE;
}

__declspec(noinline) NTSTATUS UnhookMouHidCallback() {
	if (!MouseHookCtx.OriginalMouHidCallback) return STATUS_SUCCESS;

	UNICODE_STRING driverName = { 0 };
	PDRIVER_OBJECT pMouHid = NULL;

	RtlInitUnicodeString(&driverName, L"\\Driver\\MouHID");
	if (!IoDriverObjectType || !*IoDriverObjectType) return STATUS_OBJECT_TYPE_MISMATCH;

	NTSTATUS status = ObReferenceObjectByName(&driverName, OBJ_CASE_INSENSITIVE, NULL, 0, *IoDriverObjectType, KernelMode, NULL, (PVOID*)&pMouHid);
	if (!NT_SUCCESS(status)) return status;

	BOOLEAN unhooked = FALSE;

	for (PDEVICE_OBJECT deviceObj = pMouHid->DeviceObject; deviceObj; deviceObj = deviceObj->AttachedDevice) {
		PUCHAR ext = (PUCHAR)deviceObj->DeviceExtension;
		if (!ext) continue;

		for (SIZE_T off = 0; off + sizeof(PVOID) <= 0x1FF8; off += sizeof(PVOID)) {
			PVOID* slot = (PVOID*)(ext + off);
			if (!MmIsAddressValid(slot)) continue;

			PVOID value = *slot;
			if (value == (PVOID)MouHidHookCallback) {
				*slot = MouseHookCtx.OriginalMouHidCallback;
				unhooked = TRUE;
			}
		}
	}

	ObfDereferenceObject(pMouHid);

	if (unhooked) {
		MouseHookCtx.OriginalMouHidCallback = NULL;
		MouseHookCtx.HookInstalled = FALSE;
	}

	return unhooked ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

__declspec(noinline) VOID CleanupMouseHook() {
	if (MouseHookCtx.HookInstalled) {
		UnhookMouHidCallback();
	}
	MouseHookCtx.PendingDeltaX = 0;
	MouseHookCtx.PendingDeltaY = 0;
	MouseHookCtx.Initialized = FALSE;
	MouseHookCtx.HookInstalled = FALSE;
	MouseHookCtx.OriginalMouHidCallback = NULL;
}