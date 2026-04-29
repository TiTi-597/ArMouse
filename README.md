# ArMouse
Hook `MouHID` 驱动的 `MouseClassServiceCallback` 回调函数，在鼠标数据传递给 `MouClass` 之前修改 `MOUSE_INPUT_DATA` 结构中的 `LastX` 和 `LastY` 字段，实现内核级鼠标移动。
