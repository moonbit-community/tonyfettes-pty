///|
type PtyHandle

///|
#borrow(argv_flat)
extern "c" fn pty_spawn_ffi(
  argv_flat : Bytes,
  argc : Int,
  cols : Int,
  rows : Int,
) -> PtyHandle = "moonbit_pty_spawn"

///|
#borrow(handle)
extern "c" fn pty_resize_ffi(handle : PtyHandle, cols : Int, rows : Int) -> Int = "moonbit_pty_resize"

///|
#borrow(handle)
extern "c" fn pty_take_read_fd_ffi(handle : PtyHandle) -> @types.Fd = "moonbit_pty_take_read_fd"

///|
#cfg(platform="windows")
#borrow(handle)
extern "c" fn pty_take_write_fd_ffi(handle : PtyHandle) -> @types.Fd = "moonbit_pty_take_write_fd"

///|
#borrow(handle)
extern "c" fn pty_child_pid_ffi(handle : PtyHandle) -> Int = "moonbit_pty_child_pid"

///|
#borrow(handle)
extern "c" fn pty_try_wait_ffi(handle : PtyHandle) -> Int = "moonbit_pty_try_wait"

///|
#borrow(handle)
extern "c" fn pty_close_ffi(handle : PtyHandle) -> Unit = "moonbit_pty_close"
