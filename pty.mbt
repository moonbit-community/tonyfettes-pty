///|
pub suberror PtyError {
  SpawnFailed(String)
  WriteError
  ResizeError
} derive(Debug)

///|
/// Handle to a spawned PTY plus its event-loop registration.
///
/// On Unix the PTY master fd is used for both reading and writing, so
/// `read_io` and `write_io` are the same `RawFd` wrapper around the same
/// `IoHandle`. The two fields exist so that reader and writer code paths
/// can use intent-revealing names, and so that a future Windows build
/// can point them at the separate input/output pipe HANDLEs without
/// disturbing callers.
pub struct Pty {
  priv handle : PtyHandle
  priv read_io : @raw_fd.RawFd
  priv write_io : @raw_fd.RawFd
}

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
/// Returns a `Pty` handle or raises `PtyError::SpawnFailed` on failure.
pub async fn Pty::spawn(
  argv : Array[String],
  cols? : Int = 80,
  rows? : Int = 24,
) -> Pty raise PtyError {
  if argv.length() == 0 {
    raise SpawnFailed("empty argv")
  }
  // Flatten argv into a single null-separated byte buffer:
  //   "arg0\0arg1\0...argN\0"
  // The C side walks the buffer using the supplied argc and the embedded
  // null terminators.
  let buf = @buffer.new()
  for arg in argv {
    buf.write_bytes(@utf8.encode(arg[:]))
    buf.write_byte(b'\x00')
  }
  let argv_flat = buf.contents()
  let handle = pty_spawn(argv_flat, argv.length(), cols, rows)
  // Transfer fd ownership from the C PTY handle to `RawFd`.
  let read_io = @raw_fd.RawFd::new(pty_take_read_fd(handle)) catch {
    err => {
      pty_close(handle)
      raise SpawnFailed("RawFd setup failed (read): \{@debug.to_string(err)}")
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
) -> @raw_fd.RawFd raise PtyError {
  let write_io = @raw_fd.RawFd::new(pty_take_write_fd(handle)) catch {
    err => {
      read_io.close()
      pty_close(handle)
      raise SpawnFailed("RawFd setup failed (write): \{@debug.to_string(err)}")
    }
  }
  write_io
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
/// bytes are written (or a real error occurs). Unlike the old blocking
/// implementation, this does not stall other concurrent tasks when the
/// child stops draining the PTY input buffer.
///
/// Raises `PtyError::WriteError` if the write fails.
pub async fn Pty::write(self : Pty, data : Bytes) -> Unit raise PtyError {
  let total = data.length()
  let mut offset = 0
  while offset < total {
    let n = self.write_io.write(data, offset~, len=total - offset) catch {
      _ => raise WriteError
    }
    if n <= 0 {
      raise WriteError
    }
    offset += n
  }
}

///|
/// Resize the PTY window.
///
/// Raises `PtyError::ResizeError` if the resize fails.
pub fn Pty::resize(self : Pty, cols : Int, rows : Int) -> Unit raise PtyError {
  let rc = pty_resize(self.handle, cols, rows)
  if rc < 0 {
    raise ResizeError
  }
}

///|
/// Get the spawned child PID when available.
pub fn Pty::pid(self : Pty) -> Int {
  pty_child_pid(self.handle)
}

///|
/// Return the child exit code if it has already terminated.
///
/// Returns `None` while the child is still running.
pub fn Pty::try_exit_code(self : Pty) -> Int? {
  let code = pty_try_wait(self.handle)
  if code == -2 {
    None
  } else {
    Some(code)
  }
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
