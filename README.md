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
that never triggers atfork handlers) to re-launch the current binary with
special environment variables:

1. `openpty()` creates a PTY pair (master + slave fd)
2. `posix_spawn()` launches the same executable with env vars:
   - `MOONBIT_PTY_EXEC=1` — signals helper mode
   - `MOONBIT_PTY_SLAVE_FD=<fd>` — the slave fd to use
   - `MOONBIT_PTY_ARGC=<n>` and `MOONBIT_PTY_ARG{0..n-1}=<arg>` — the child argv
3. A `__attribute__((constructor))` function in the child checks for
   `MOONBIT_PTY_EXEC=1`, calls `login_tty()` + `execvp()`, and `_exit()`s
   before `main()` ever runs

### Why argv must be copied

The parent copies the full user `argv` into env vars, and also re-spawns with
the parent's own `argc`/`argv` as `posix_spawn()` arguments. This is **required**
because in MoonBit debug mode, `_NSGetExecutablePath()` returns the path to
`tcc` (MoonBit's interpreter), not the test/application binary. Re-spawning
`tcc` without the original arguments means it doesn't know which compiled
module to load — it would exit immediately without ever running the
constructor that intercepts and execs the shell.

In release mode, MoonBit compiles to a standalone native binary, so
`_NSGetExecutablePath()` returns the actual binary with the constructor baked
in. The extra argv is harmless in that case (the constructor fires before
`main()` parses them).
