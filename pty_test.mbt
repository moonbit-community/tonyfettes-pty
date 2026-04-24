///|
// These tests spawn POSIX-only programs (`/bin/sh`, `/bin/cat`, bare `sh`).
// They're skipped on Windows where the equivalents would be `cmd.exe` and
// the ConPTY code path isn't covered here yet — see TODO in pty_stub.c.

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
  let reader = pty.reader()
  let buf : FixedArray[Byte] = FixedArray::make(256, b'\x00')

  pty.write(@utf8.encode("ping\n"))
  let n = @async.with_timeout(1000, () => reader.read(buf))
  assert_true(n > 0)

  let output = @utf8.decode(Bytes::from_array(buf[:n]))
  assert_true(output.contains("ping"))
}

///|
#cfg(not(platform="windows"))
async test "sh accepts commands through PTY" {
  let pty = Pty::spawn(["/bin/sh"])
  defer pty.close()
  let reader = pty.reader()
  let buf : FixedArray[Byte] = FixedArray::make(256, b'\x00')

  pty.write(@utf8.encode("echo READY\nexit\n"))

  let mut output = ""
  let mut found_ready = false
  @async.with_timeout(1000, () => {
    for _ in 0..<8 {
      let n = reader.read(buf)
      if n == 0 {
        break
      }
      output = output + @utf8.decode(Bytes::from_array(buf[:n]))
      if output.contains("READY") {
        found_ready = true
        break
      }
    }
  })

  assert_true(found_ready)
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
  let reader = pty.reader()
  let buf : FixedArray[Byte] = FixedArray::make(256, b'\x00')

  let mut output = ""
  let mut found = false
  @async.with_timeout(1000, () => {
    for _ in 0..<8 {
      let n = reader.read(buf)
      if n <= 0 {
        break
      }
      output = output + @utf8.decode(Bytes::from_array(buf[:n]))
      if output.contains("ARGV_OK") {
        found = true
        break
      }
    }
  })

  assert_true(found)
}

///|
/// Spawn a program by bare name (not absolute path). This exercises the
/// PATH lookup we're adding — `execvp` must resolve `sh` via $PATH.
#cfg(not(platform="windows"))
async test "spawn resolves program via PATH" {
  let pty = Pty::spawn(["sh", "-c", "echo PATH_OK"])
  defer pty.close()
  let reader = pty.reader()
  let buf : FixedArray[Byte] = FixedArray::make(256, b'\x00')

  let mut output = ""
  let mut found = false
  @async.with_timeout(1000, () => {
    for _ in 0..<8 {
      let n = reader.read(buf)
      if n <= 0 {
        break
      }
      output = output + @utf8.decode(Bytes::from_array(buf[:n]))
      if output.contains("PATH_OK") {
        found = true
        break
      }
    }
  })

  assert_true(found)
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
