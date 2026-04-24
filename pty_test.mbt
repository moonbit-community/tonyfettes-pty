///|
// These tests spawn POSIX-only programs (`/bin/sh`, `/bin/cat`, bare `sh`).
// They're skipped on Windows where the equivalents would be `cmd.exe` and
// the ConPTY code path isn't covered here yet — see TODO in pty_stub.c.

///|
/// Read from the PTY until `needle` appears in the accumulated output or
/// the timeout fires. Returns `Some(output)` if the needle appeared, `None`
/// otherwise.
async fn read_until_contains(
  pty : Pty,
  needle : String,
  timeout_ms~ : Int = 1000,
  max_reads~ : Int = 8,
) -> String? {
  let reader = pty.reader()
  let buf : FixedArray[Byte] = FixedArray::make(256, b'\x00')
  let mut output = ""
  let mut found = false
  @async.with_timeout(timeout_ms, () => for _ in 0..<max_reads {
    let n = reader.read(buf)
    if n <= 0 {
      break
    }
    output = output + @utf8.decode(Bytes::from_array(buf[:n]))
    if output.contains(needle) {
      found = true
      break
    }
  })
  if found {
    Some(output)
  } else {
    None
  }
}

///|
#cfg(not(platform="windows"))
async test "spawn succeeds and reports a child pid" {
  let pty = Pty::spawn(["/bin/sh"])
  defer pty.close()
  assert_true(pty.pid() > 0)
}

///|
#cfg(not(platform="windows"))
async test "cat echoes input through PTY" {
  let pty = Pty::spawn(["/bin/cat"])
  defer pty.close()
  pty.write(@utf8.encode("ping\n"))
  assert_true(read_until_contains(pty, "ping") is Some(_))
}

///|
#cfg(not(platform="windows"))
async test "sh accepts commands through PTY" {
  let pty = Pty::spawn(["/bin/sh"])
  defer pty.close()
  pty.write(@utf8.encode("echo READY\nexit\n"))
  assert_true(read_until_contains(pty, "READY") is Some(_))
}

///|
#cfg(not(platform="windows"))
async test "idle PTY read waits instead of EOF" {
  let pty = Pty::spawn(["/bin/cat"])
  defer pty.close()
  let reader = pty.reader()
  let buf : FixedArray[Byte] = FixedArray::make(64, b'\x00')
  let result = try? @async.with_timeout(100, () => reader.read(buf))
  inspect(result, content="Err(TimeoutError)")
}

///|
/// Spawn a program with a multi-argument argv and verify the args were
/// actually forwarded to the child. Uses `sh -c 'echo ARGV_OK'` because
/// that proves the child received at least two args and interpreted the
/// second one as a command.
#cfg(not(platform="windows"))
async test "spawn forwards argv beyond argv[0]" {
  let pty = Pty::spawn(["/bin/sh", "-c", "echo ARGV_OK"])
  defer pty.close()
  assert_true(read_until_contains(pty, "ARGV_OK") is Some(_))
}

///|
/// Spawn a program by bare name (not absolute path). This exercises the
/// PATH lookup we're adding — `execvp` must resolve `sh` via $PATH.
#cfg(not(platform="windows"))
async test "spawn resolves program via PATH" {
  let pty = Pty::spawn(["sh", "-c", "echo PATH_OK"])
  defer pty.close()
  assert_true(read_until_contains(pty, "PATH_OK") is Some(_))
}

///|
/// Spawning a program that doesn't exist must surface the exec failure
/// as an OSError, not silently return a live-looking Pty whose child
/// already died. Exercises the error-reporting pipe from the child
/// helper (macOS) / post-fork exec path (Linux).
#cfg(not(platform="windows"))
async test "spawn reports exec failure for missing program" {
  let err = try {
    let pty = Pty::spawn(["/nonexistent/program/xyzzy"])
    pty.close()
    None
  } catch {
    e => Some(e)
  }
  inspect(
    err,
    content=(
      #|Some(OSError("Pty::spawn: No such file or directory"))
    ),
  )
}
