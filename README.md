# tonyfettes/pty

Cross-platform PTY (pseudo-terminal) spawning for MoonBit native targets,
integrated with `moonbitlang/async` so reads and writes go through the async
event loop instead of blocking the thread.

Extracted from `tonyfettes/tun-poc-server`'s `server/pty` package.

## API

```moonbit
let pty = @pty.Pty::spawn(["/bin/sh", "-c", "echo hello"])
defer pty.close()

let reader = pty.reader()                 // @raw_fd.RawFd
pty.write(@utf8.encode("ls\n"))           // async
pty.resize(120, 40)
let pid : Int = pty.pid()
```

`Pty::spawn` registers the master fd with the async event loop, so it must be
called from inside an async context. `argv[0]` is resolved via `PATH`
(`execvp`).

## Errors

Failures are reported as `@moonbitlang/async/os_error.OSError(code, context~)`,
where `code` is `errno` on Unix or `GetLastError()` on Windows. Use the
package's `is_EACCES`, `is_ENOENT`, `is_nonblocking_io_error`, etc. predicates
to branch on specific kinds:

```moonbit
try {
  @pty.Pty::spawn(["/bin/missing"])
} catch {
  err is @os_error.OSError if err.is_ENOENT() => ...
  err => raise err
}
```

## Platform strategies

| Platform | Method | Why |
|----------|--------|-----|
| macOS | `openpty()` + `posix_spawn()` self-helper | `fork()` crashes in mimalloc's atfork child handler |
| Linux | `forkpty()` + `execvp()` | Direct fork works (no macOS malloc zone mechanism) |
| Windows | ConPTY + `CreateProcessA()` | No fork involved |

## macOS: the mimalloc + fork problem

MoonBit's release builds ship with mimalloc as the default allocator. On macOS,
mimalloc registers itself as a custom malloc zone. When `fork()` is called,
`libSystem_atfork_child` iterates all registered malloc zones in the child and
calls their introspection callbacks. mimalloc's `mi_introspect` struct has a
NULL function pointer for one of these callbacks, causing the child to segfault
(signal 11, exit code 139) before it ever reaches `exec()`.

Reproduction results (linking a minimal C `forkpty()` program with/without
`libmoonbitrun.o`):

| Variant | Without runtime | With libmoonbitrun.o |
|---------|----------------|---------------------|
| `forkpty()` on main thread | works | child exits 139 (SIGSEGV) |
| `forkpty()` in a pthread | works | child exits 139 (SIGSEGV) |
| `posix_spawn()` self-helper | works | works |
| MoonBit release without mimalloc | works | n/a |

A pthread does not help, because `pthread_atfork` child handlers are
process-wide — the child inherits the same malloc zones with the same NULL
function pointer regardless of which thread called `fork()`.

## macOS: the self-helper pattern

Since `fork()` is off limits, we use `posix_spawn()` (kernel-level spawn path
that never triggers atfork handlers) to re-launch the current binary and use a
`__attribute__((constructor))` function to intercept before `main()` runs.

Plumbing between parent and helper is three env vars plus two inherited pipes:

1. `openpty()` creates a PTY pair (master + slave fd).
2. The parent creates two pipes — `argv_pipe` (parent → helper) and `err_pipe`
   (helper → parent) — and passes the child-side fds to `posix_spawn()` via
   `posix_spawn_file_actions_addinherit_np()` under `POSIX_SPAWN_CLOEXEC_DEFAULT`.
3. `posix_spawn()` launches the same executable with env vars:
   - `MOONBIT_PTY_EXEC=1` — signals helper mode
   - `MOONBIT_PTY_SLAVE_FD=<fd>` — the slave fd to use
   - `MOONBIT_PTY_ARGV_FD=<fd>` — read end of the argv pipe
   - `MOONBIT_PTY_ERR_FD=<fd>` — write end of the error pipe
4. The parent streams the target `argv` (flattened as `arg0\0arg1\0…argN\0`)
   into `argv_pipe` and closes its write end.
5. The constructor reads argv from `argv_pipe` until EOF, calls `login_tty()`,
   arms `FD_CLOEXEC` on its copy of the error pipe, and `execvp()`s. A
   successful exec auto-closes the error pipe so the parent sees EOF; any
   pre-exec failure writes the errno to the pipe and `_exit()`s.
6. The parent reads from the error pipe: 4 bytes → failure with that errno,
   EOF → success.

### Why argv must be copied

The parent also re-spawns with its own `argc`/`argv` as `posix_spawn()`
arguments (separate from the target argv that gets streamed through the pipe).
This is **required** because in MoonBit debug mode, `_NSGetExecutablePath()`
returns the path to `tcc` (MoonBit's interpreter), not the test/application
binary. Re-spawning `tcc` without the original arguments means it doesn't know
which compiled module to load — it would exit immediately without ever running
the constructor that intercepts and execs the shell.

In release mode, MoonBit compiles to a standalone native binary, so
`_NSGetExecutablePath()` returns the actual binary with the constructor baked
in. The extra argv is harmless in that case (the constructor fires before
`main()` parses them).

## Linux: forkpty with an error pipe

Linux uses `forkpty()` + `execvp()` directly. A `pipe2(O_CLOEXEC)` error pipe
wraps the fork the same way as the macOS helper: on exec success the pipe
auto-closes (EOF = success); on failure the child writes its errno before
`_exit()`ing.
