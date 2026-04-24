///|
/// Handle to a spawned PTY plus its event-loop registration.
///
/// On Unix the PTY master fd is used for both reading and writing, so
/// `read_io` and `write_io` are the same `RawFd` wrapper around the same
/// `IoHandle`. The two fields exist so that reader and writer code paths
/// can use intent-revealing names, and so that a future Windows build
/// can point them at the separate input/output pipe HANDLEs without
/// disturbing callers.
struct Pty {
  handle : PtyHandle
  read_io : @raw_fd.RawFd
  write_io : @raw_fd.RawFd
}

///|
/// POSIX `EINVAL`. Used as the error code when `argv` is empty —
/// `@os_error.errno_to_string` renders it as "Invalid argument".
const EINVAL : Int = 22

///|
/// Spawn a new PTY running the given program.
///
/// `argv[0]` is the program to execute (resolved via PATH by `execvp`),
/// and `argv[1..]` are its arguments.
///
/// `cols` and `rows` default to 80x24 if not specified.
///
/// Must be called from inside an async event-loop context — the PTY's
/// master fd is registered with the event loop on construction so that
/// reads and writes can suspend instead of blocking the thread.
///
/// Raises `@os_error.OSError` wrapping `errno` (Unix) or `GetLastError()`
/// (Windows) if the underlying syscall fails.
pub async fn Pty::spawn(
  argv : Array[String],
  cols? : Int = 80,
  rows? : Int = 24,
) -> Pty {
  if argv.is_empty() {
    raise @os_error.OSError(EINVAL, context="Pty::spawn: empty argv")
  }
  // Flatten argv into a single null-separated byte buffer:
  //   "arg0\0arg1\0...argN\0"
  // The C side walks the buffer by null byte, using the buffer's own
  // length to know when to stop.
  let buf = @buffer.new()
  for arg in argv {
    buf.write_bytes(@utf8.encode(arg[:]))
    buf.write_byte(b'\x00')
  }
  let argv_flat = buf.contents()
  let handle = pty_spawn(argv_flat, cols, rows)
  let spawn_err = pty_check_spawn(handle)
  if spawn_err != 0 {
    pty_close(handle)
    raise @os_error.OSError(spawn_err, context="Pty::spawn")
  }
  // Transfer fd ownership from the C PTY handle to `RawFd`.
  let read_io = @raw_fd.RawFd::new(pty_take_read_fd(handle)) catch {
    err => {
      pty_close(handle)
      raise err
    }
  }
  let write_io = register_write_io(handle, read_io)
  { handle, read_io, write_io }
}

///|
/// On Unix read and write share the same PTY master fd.
#cfg(not(platform="windows"))
fn register_write_io(
  _handle : PtyHandle,
  read_io : @raw_fd.RawFd,
) -> @raw_fd.RawFd {
  read_io
}

///|
/// On Windows ConPTY has a separate input pipe HANDLE.
#cfg(platform="windows")
fn register_write_io(
  handle : PtyHandle,
  read_io : @raw_fd.RawFd,
) -> @raw_fd.RawFd {
  @raw_fd.RawFd::new(pty_take_write_fd(handle)) catch {
    err => {
      read_io.close()
      pty_close(handle)
      raise err
    }
  }
}

///|
/// Get the async reader for this PTY.
///
/// The returned `RawFd` is owned by the `Pty`; do not call `close()`
/// or `detach()` on it. Use `Pty::close` to release the registration.
pub fn Pty::reader(self : Pty) -> @raw_fd.RawFd {
  self.read_io
}

///|
/// Write data to the PTY stdin.
///
/// Suspends the current coroutine via the async event loop until all
/// bytes are written, so a stuck child that stops draining its input
/// buffer won't block other concurrent tasks in the runtime.
///
/// Raises `@os_error.OSError` if the underlying write fails.
pub async fn Pty::write(self : Pty, data : Bytes) -> Unit {
  let total = data.length()
  let mut offset = 0
  while offset < total {
    let n = self.write_io.write(data, offset~, len=total - offset)
    if n <= 0 {
      raise @os_error.OSError(EINVAL, context="Pty::write: short write")
    }
    offset += n
  }
}

///|
/// Resize the PTY window.
///
/// Raises `@os_error.OSError` if the underlying syscall fails.
pub fn Pty::resize(
  self : Pty,
  cols : Int,
  rows : Int,
) -> Unit raise @os_error.OSError {
  // Explicitly narrowed: pty_resize is the only source of error here.
  let err = pty_resize(self.handle, cols, rows)
  if err != 0 {
    raise @os_error.OSError(err, context="Pty::resize")
  }
}

///|
/// Get the spawned child PID when available.
pub fn Pty::pid(self : Pty) -> Int {
  pty_child_pid(self.handle)
}

///|
/// Close the PTY, releasing all resources.
///
/// Closes the owned `RawFd` wrapper(s) and then tears down the underlying
/// OS PTY handle.
///
/// Safe to call multiple times because the underlying close paths are
/// idempotent.
#cfg(not(platform="windows"))
pub fn Pty::close(self : Pty) -> Unit {
  self.read_io.close()
  pty_close(self.handle)
}

///|
/// Close the PTY, releasing all resources.
///
/// Closes the owned `RawFd` wrapper(s) and then tears down the underlying
/// OS PTY handle.
///
/// Safe to call multiple times because the underlying close paths are
/// idempotent.
#cfg(platform="windows")
pub fn Pty::close(self : Pty) -> Unit {
  self.read_io.close()
  self.write_io.close()
  pty_close(self.handle)
}
