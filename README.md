# ArMouse

Windows 内核级鼠标模拟驱动程序

## 项目简介

ArMouse 是一个 Windows 内核驱动程序，通过 Hook MouHID 驱动的回调函数实现内核级鼠标移动模拟。

## 实现原理

### 核心思路

Windows 鼠标输入流程：`硬件 → MouHID → MouClass → 用户态`

本驱动通过 Hook `MouHID` 驱动的 `MouseClassServiceCallback` 回调函数，在鼠标数据传递给 `MouClass` 之前修改 `MOUSE_INPUT_DATA` 结构中的 `LastX` 和 `LastY` 字段，实现内核级鼠标移动。

### 关键步骤

#### 1. 获取驱动对象

```c
RtlInitUnicodeString(&driverName, L"\\Driver\\MouClass");
ObReferenceObjectByName(&driverName, ..., &pMouClass);

RtlInitUnicodeString(&driverName, L"\\Driver\\MouHID");
ObReferenceObjectByName(&driverName, ..., &pMouHid);
```

#### 2. 定位回调函数指针

遍历 `MouHID` 设备对象的 `DeviceExtension`，查找指向 `MouClass` 驱动内存范围内的函数指针：

```c
for (PDEVICE_OBJECT deviceObj = pMouHid->DeviceObject; deviceObj; deviceObj = deviceObj->AttachedDevice) {
    PUCHAR ext = (PUCHAR)deviceObj->DeviceExtension;
    for (SIZE_T off = 0; off < 0x1FF8; off += sizeof(PVOID)) {
        PVOID value = *(PVOID*)(ext + off);
        if (value 在 MouClass 地址范围内) {
            // 找到 MouseClassServiceCallback 指针
        }
    }
}
```

#### 3. 替换回调函数

保存原始回调地址，替换为自定义 Hook 函数：

```c
MouseHookCtx.OriginalMouHidCallback = value;  // 保存原始
*slot = (PVOID)MouHidHookCallback;            // 替换为 Hook
```

#### 4. Hook 回调函数实现

```c
VOID MouHidHookCallback(PDEVICE_OBJECT DeviceObject, PMOUSE_INPUT_DATA InputStart, PMOUSE_INPUT_DATA InputEnd, PULONG InputConsumed) {
    // 获取待注入的移动量
    LONG dx = MouseHookCtx.PendingDeltaX;
    LONG dy = MouseHookCtx.PendingDeltaY;
    
    // 修改鼠标数据
    for (PMOUSE_INPUT_DATA cur = InputStart; cur < InputEnd; ++cur) {
        cur->LastX += dx;
        cur->LastY += dy;
    }
    
    // 调用原始回调
    ((MouseClassServiceCallback)MouseHookCtx.OriginalMouHidCallback)(DeviceObject, InputStart, InputEnd, InputConsumed);
}
```

#### 5. 队列移动量

```c
BOOLEAN QueueMouseDelta(LONG dx, LONG dy) {
    KIRQL oldIrql = AcquireSpinLock(&MouseHookCtx.PendingDeltaSpinLock);
    MouseHookCtx.PendingDeltaX += dx;
    MouseHookCtx.PendingDeltaY += dy;
    ReleaseSpinLock(&MouseHookCtx.PendingDeltaSpinLock);
    return TRUE;
}
```

### 数据结构

```c
typedef struct _MOUSE_INPUT_DATA {
    USHORT UnitId;
    USHORT Flags;
    union {
        ULONG Buttons;
        struct {
            USHORT ButtonFlags;
            USHORT ButtonData;
        };
    };
    ULONG RawButtons;
    LONG LastX;      // X轴移动量
    LONG LastY;      // Y轴移动量
    ULONG ExtraInformation;
} MOUSE_INPUT_DATA;
```

## 编译要求

- Visual Studio 2019+
- Windows Driver Kit (WDK)

## 免责声明

本项目仅供学习研究目的，请自行承担使用风险。

## 许可证

MIT License