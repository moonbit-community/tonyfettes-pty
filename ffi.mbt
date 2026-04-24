///|
priv type PtyHandle

///|
#borrow(argv_flat)
extern "c" fn pty_spawn(argv_flat : Bytes, cols : Int, rows : Int) -> PtyHandle = "moonbit_pty_spawn"

///|
/// Returns the OS error captured during `pty_spawn` (0 = success).
/// The value is `errno` on Unix / `GetLastError()` on Windows.
#borrow(handle)
extern "c" fn pty_check_spawn(handle : PtyHandle) -> Int = "moonbit_pty_check_spawn"

///|
#borrow(handle)
extern "c" fn pty_resize(handle : PtyHandle, cols : Int, rows : Int) -> Int = "moonbit_pty_resize"

///|
#borrow(handle)
extern "c" fn pty_take_read_fd(handle : PtyHandle) -> @types.Fd = "moonbit_pty_take_read_fd"

///|
#cfg(platform="windows")
#borrow(handle)
extern "c" fn pty_take_write_fd(handle : PtyHandle) -> @types.Fd = "moonbit_pty_take_write_fd"

///|
#borrow(handle)
extern "c" fn pty_child_pid(handle : PtyHandle) -> Int = "moonbit_pty_child_pid"

///|
#borrow(handle)
extern "c" fn pty_close(handle : PtyHandle) -> Unit = "moonbit_pty_close"
