/*
 * pty_stub.c — Cross-platform PTY implementation for MoonBit FFI.
 *
 * Unix:    forkpty() + fcntl(O_NONBLOCK) + ioctl(TIOCSWINSZ)
 * Windows: ConPTY (dynamically loaded from kernel32.dll)
 *
 * All exported functions use MOONBIT_FFI_EXPORT and follow the
 * MoonBit external-object pattern (moonbit_make_external_object).
 */

#include <moonbit.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/*  Internal handle structure                                                 */
/* -------------------------------------------------------------------------- */

typedef struct pty_handle {
#ifdef _WIN32
  void *hpc;            /* HPCON */
  void *pipe_in_read;   /* stdin  pipe: read  end */
  void *pipe_in_write;  /* stdin  pipe: write end */
  void *pipe_out_read;  /* stdout pipe: read  end */
  void *pipe_out_write; /* stdout pipe: write end */
  void *proc_handle;    /* child PROCESS_INFORMATION.hProcess */
  void *thread_handle;  /* child PROCESS_INFORMATION.hThread  */
#else
  int master_fd;
  int spawned_pid;
  int child_pid;
  int child_exited;
  int child_status;
#endif
} pty_handle_t;

/*
 * MoonBitPty is the external-object wrapper seen by MoonBit.
 * moonbit_make_external_object allocates (payload_size) bytes after
 * the GC header; our payload is (pty_handle_t *, spawn_errno).
 *
 * `spawn_errno` is 0 on success, otherwise the errno / GetLastError
 * captured at the point moonbit_pty_spawn failed. On failure, `handle`
 * is NULL, but the MoonBitPty object itself is still valid so MoonBit
 * can call moonbit_pty_check_spawn / moonbit_pty_close on it.
 */
typedef struct {
  pty_handle_t *handle;
  int32_t spawn_errno;
} MoonBitPty;

/* Forward declaration of the platform close helper. */
static void
pty_close_impl(pty_handle_t *h);
static void
moonbit_pty_finalizer(void *ptr);

/* Allocate a MoonBitPty representing a failed spawn. `handle` stays NULL;
 * `err` stores the captured OS error (errno on Unix, GetLastError on
 * Windows) so MoonBit can report it via moonbit_pty_check_spawn. */
static MoonBitPty *
pty_make_failure(int32_t err) {
  MoonBitPty *p = (MoonBitPty *)moonbit_make_external_object(
    moonbit_pty_finalizer, sizeof(MoonBitPty)
  );
  p->handle = NULL;
  p->spawn_errno = err;
  return p;
}

/* Allocate a MoonBitPty wrapping a successfully-initialized handle. */
static MoonBitPty *
pty_make_success(pty_handle_t *h) {
  MoonBitPty *p = (MoonBitPty *)moonbit_make_external_object(
    moonbit_pty_finalizer, sizeof(MoonBitPty)
  );
  p->handle = h;
  p->spawn_errno = 0;
  return p;
}

/*
 * Return the OS error captured during moonbit_pty_spawn. 0 = success.
 *
 * The value is errno on Unix / GetLastError on Windows (same convention
 * as @os_error.get_errno), so MoonBit can wrap it directly in an OSError.
 */
MOONBIT_FFI_EXPORT
int32_t
moonbit_pty_check_spawn(MoonBitPty *p) {
  if (!p)
    return 0;
  return p->spawn_errno;
}

/* -------------------------------------------------------------------------- */
/*  Cross-platform argv plumbing                                              */
/* -------------------------------------------------------------------------- */

/*
 * Parse the flattened argv buffer into a NULL-terminated char** array.
 *
 * Wire format: "arg0\0arg1\0...arg(argc-1)\0"
 * `argc` gives the number of arguments; the null bytes separate them.
 *
 * On success returns a newly-allocated argv array that the caller must free
 * with pty_free_argv. Returns NULL on malformed input or allocation failure.
 *
 * Defined here (above the #ifdef split) so both the POSIX forkpty path and
 * the Windows ConPTY path can call it.
 */
static char **
pty_parse_argv_flat(const uint8_t *argv_flat, int32_t argc) {
  if (argc <= 0 || !argv_flat) {
    return NULL;
  }
  int32_t flat_len = (int32_t)Moonbit_array_length(argv_flat);
  if (flat_len <= 0) {
    return NULL;
  }

  char **out = (char **)calloc((size_t)argc + 1, sizeof(char *));
  if (!out) {
    return NULL;
  }

  int32_t pos = 0;
  for (int i = 0; i < argc; i++) {
    if (pos >= flat_len) {
      goto fail; /* Ran out of buffer before reading argc args */
    }
    int32_t end = pos;
    while (end < flat_len && argv_flat[end] != 0) {
      end++;
    }
    if (end >= flat_len) {
      goto fail; /* Missing null terminator */
    }
    int32_t len = end - pos;
    char *str = (char *)malloc((size_t)len + 1);
    if (!str) {
      goto fail;
    }
    memcpy(str, argv_flat + pos, (size_t)len);
    str[len] = '\0';
    out[i] = str;
    pos = end + 1;
  }
  out[argc] = NULL;
  return out;

fail:
  for (int i = 0; i < argc; i++) {
    if (out[i])
      free(out[i]);
  }
  free(out);
  return NULL;
}

static void
pty_free_argv(char **argv) {
  if (!argv)
    return;
  for (int i = 0; argv[i]; i++) {
    free(argv[i]);
  }
  free(argv);
}

/* -------------------------------------------------------------------------- */
/*  Finalizer (invoked by GC)                                                 */
/* -------------------------------------------------------------------------- */

static void
moonbit_pty_finalizer(void *ptr) {
  MoonBitPty *p = (MoonBitPty *)ptr;
  if (p->handle) {
    pty_close_impl(p->handle);
    free(p->handle);
    p->handle = NULL;
  }
}

/* ========================================================================== */
/*  UNIX IMPLEMENTATION                                                       */
/* ========================================================================== */
#ifndef _WIN32

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

/* forkpty() lives in different headers depending on the platform. */
#if defined(__APPLE__)
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#else
#include <pty.h>
#endif

#if defined(__APPLE__)
#define MOONBIT_PTY_EXEC_ENV "MOONBIT_PTY_EXEC"
#define MOONBIT_PTY_SLAVE_FD_ENV "MOONBIT_PTY_SLAVE_FD"
#define MOONBIT_PTY_ARGV_FD_ENV "MOONBIT_PTY_ARGV_FD"
#define MOONBIT_PTY_ERR_FD_ENV "MOONBIT_PTY_ERR_FD"
#endif

static void
pty_set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

/* Blocking write of exactly `len` bytes of `errno` back to the parent.
 * Retries on EINTR and short writes. The return value is intentionally
 * ignored by callers: we're on a fatal path and if the error pipe is
 * broken the parent will see EOF and fall back to its generic failure
 * path. */
static void
pty_report_error(int err_fd, int err) {
  int32_t val = (int32_t)err;
  const char *p = (const char *)&val;
  size_t remaining = sizeof(val);
  while (remaining > 0) {
    ssize_t n = write(err_fd, p, remaining);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return;
    }
    p += n;
    remaining -= (size_t)n;
  }
}

/* Blocking write of exactly `len` bytes from buf. Retries on EINTR and
 * short writes. Returns 0 on full success, errno on failure (including
 * EPIPE when the child died before reading). */
static int
pty_write_all(int fd, const void *buf, size_t len) {
  const char *p = (const char *)buf;
  while (len > 0) {
    ssize_t n = write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return errno;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

/* Read exactly one int32 errno from the child's error pipe. Returns 1 if
 * the child reported an errno (stored in *out), 0 on clean EOF (success
 * — the write end was auto-closed on exec via FD_CLOEXEC), -1 on an
 * unexpected read error or partial write. */
static int
pty_read_child_error(int fd, int32_t *out) {
  char *p = (char *)out;
  size_t remaining = sizeof(*out);
  size_t total = 0;
  while (remaining > 0) {
    ssize_t n = read(fd, p, remaining);
    if (n == 0)
      break;
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    p += n;
    remaining -= (size_t)n;
    total += (size_t)n;
  }
  if (total == sizeof(*out))
    return 1;
  if (total == 0)
    return 0;
  return -1;
}

#if defined(__APPLE__)
static char *
pty_get_self_executable_path(void) {
  uint32_t size = 0;
  if (_NSGetExecutablePath(NULL, &size) != -1 || size == 0) {
    return NULL;
  }
  char *path = (char *)malloc((size_t)size);
  if (!path) {
    return NULL;
  }
  if (_NSGetExecutablePath(path, &size) != 0) {
    free(path);
    return NULL;
  }
  return path;
}

/* Constructor-side unsetenv so the target program doesn't inherit our
 * plumbing vars. Argv is streamed through a pipe rather than encoded
 * per-arg, so the set of helper vars is now fixed and small. */
static void
pty_unset_helper_env(void) {
  unsetenv(MOONBIT_PTY_EXEC_ENV);
  unsetenv(MOONBIT_PTY_SLAVE_FD_ENV);
  unsetenv(MOONBIT_PTY_ARGV_FD_ENV);
  unsetenv(MOONBIT_PTY_ERR_FD_ENV);
}

/* Read the flattened argv buffer from argv_fd until EOF. Wire format is
 * the same null-separated buffer the MoonBit side builds:
 *   "arg0\0arg1\0...arg(n-1)\0"
 * Returns 0 on success with *out_buf / *out_len populated, or an errno
 * on failure. Caller owns *out_buf on success. */
static int
pty_read_argv_buf(int argv_fd, char **out_buf, size_t *out_len) {
  size_t cap = 256;
  size_t len = 0;
  char *buf = (char *)malloc(cap);
  if (!buf)
    return ENOMEM;
  for (;;) {
    if (len == cap) {
      size_t new_cap = cap * 2;
      char *new_buf = (char *)realloc(buf, new_cap);
      if (!new_buf) {
        free(buf);
        return ENOMEM;
      }
      buf = new_buf;
      cap = new_cap;
    }
    ssize_t n = read(argv_fd, buf + len, cap - len);
    if (n == 0)
      break;
    if (n < 0) {
      if (errno == EINTR)
        continue;
      int saved = errno;
      free(buf);
      return saved;
    }
    len += (size_t)n;
  }
  *out_buf = buf;
  *out_len = len;
  return 0;
}

/* Split the flat argv buffer in-place into a NULL-terminated argv array.
 * The buffer is already a sequence of null-terminated strings, so the
 * argv entries point directly into it. The buffer must stay alive until
 * execvp runs. */
static char **
pty_argv_from_buf(char *buf, size_t len) {
  if (len == 0 || buf[len - 1] != '\0')
    return NULL;
  int argc = 0;
  for (size_t i = 0; i < len; i++) {
    if (buf[i] == '\0')
      argc++;
  }
  if (argc == 0)
    return NULL;
  char **argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
  if (!argv)
    return NULL;
  int idx = 0;
  char *p = buf;
  for (size_t i = 0; i < len; i++) {
    if (buf[i] == '\0') {
      argv[idx++] = p;
      p = buf + i + 1;
    }
  }
  argv[argc] = NULL;
  return argv;
}

/* Helper-mode body: read argv from argv_fd, attach the slave tty, execvp
 * the target. Any pre-exec failure writes the errno to err_fd so the
 * parent can surface it; a successful execvp auto-closes err_fd via
 * FD_CLOEXEC, signalling EOF = success to the parent. */
static int
pty_exec_from_pipe(int argv_fd, int err_fd, int slave_fd) {
  sigset_t all_signals;
  sigfillset(&all_signals);
  sigprocmask(SIG_UNBLOCK, &all_signals, NULL);
  signal(SIGPIPE, SIG_DFL);

  char *buf = NULL;
  size_t buf_len = 0;
  int err = pty_read_argv_buf(argv_fd, &buf, &buf_len);
  close(argv_fd);
  if (err != 0) {
    pty_report_error(err_fd, err);
    return 127;
  }

  char **child_argv = pty_argv_from_buf(buf, buf_len);
  if (!child_argv) {
    pty_report_error(err_fd, EINVAL);
    free(buf);
    return 127;
  }

  pty_unset_helper_env();

  if (login_tty(slave_fd) < 0) {
    pty_report_error(err_fd, errno);
    return 126;
  }

  /* Ensure a successful execvp closes err_fd — the parent uses that EOF
   * as its "exec succeeded" signal. addinherit_np under
   * POSIX_SPAWN_CLOEXEC_DEFAULT clears FD_CLOEXEC on inherited fds in
   * the new process, so we re-arm it here just before execvp. If execvp
   * fails, the fd stays open (FD_CLOEXEC only acts on successful exec)
   * and we can still report the errno below. */
  fcntl(err_fd, F_SETFD, FD_CLOEXEC);

  execvp(child_argv[0], child_argv);
  /* execvp only returns on failure. */
  pty_report_error(err_fd, errno);
  return 127;
}

/* Parse a non-negative int fd from a decimal env-var string. Returns
 * the fd on success, or -1 if the string is missing / malformed / out
 * of range. */
static int
pty_parse_fd_env(const char *s) {
  if (!s)
    return -1;
  char *end = NULL;
  long v = strtol(s, &end, 10);
  if (!end || *end != '\0' || v < 0 || v > INT32_MAX)
    return -1;
  return (int)v;
}

__attribute__((constructor)) static void
moonbit_pty_constructor(void) {
  const char *helper_mode = getenv(MOONBIT_PTY_EXEC_ENV);
  if (!helper_mode || strcmp(helper_mode, "1") != 0) {
    return;
  }

  int slave_fd = pty_parse_fd_env(getenv(MOONBIT_PTY_SLAVE_FD_ENV));
  int argv_fd = pty_parse_fd_env(getenv(MOONBIT_PTY_ARGV_FD_ENV));
  int err_fd = pty_parse_fd_env(getenv(MOONBIT_PTY_ERR_FD_ENV));
  if (slave_fd < 0 || argv_fd < 0 || err_fd < 0) {
    _exit(127);
  }

  _exit(pty_exec_from_pipe(argv_fd, err_fd, slave_fd));
}

static int
pty_is_helper_env_name(const char *entry) {
  if (strncmp(entry, MOONBIT_PTY_EXEC_ENV "=", sizeof(MOONBIT_PTY_EXEC_ENV)) ==
      0)
    return 1;
  if (strncmp(
        entry, MOONBIT_PTY_SLAVE_FD_ENV "=", sizeof(MOONBIT_PTY_SLAVE_FD_ENV)
      ) == 0)
    return 1;
  if (strncmp(
        entry, MOONBIT_PTY_ARGV_FD_ENV "=", sizeof(MOONBIT_PTY_ARGV_FD_ENV)
      ) == 0)
    return 1;
  if (strncmp(
        entry, MOONBIT_PTY_ERR_FD_ENV "=", sizeof(MOONBIT_PTY_ERR_FD_ENV)
      ) == 0)
    return 1;
  return 0;
}

static char **
pty_make_spawn_argv(const char *self_path) {
  int argc = *_NSGetArgc();
  char **current_argv = *_NSGetArgv();
  int child_argc = argc > 0 ? argc : 1;
  char **child_argv = (char **)calloc((size_t)child_argc + 1, sizeof(char *));
  if (!child_argv) {
    return NULL;
  }
  if (argc > 0 && current_argv) {
    for (int i = 0; i < argc; i++) {
      child_argv[i] = current_argv[i];
    }
  } else {
    child_argv[0] = (char *)self_path;
  }
  child_argv[child_argc] = NULL;
  return child_argv;
}

/*
 * Build the environment for the self-exec'd helper child.
 *
 * The child inherits everything from the parent except our own helper vars,
 * which we strip and re-emit fresh. The target argv is streamed through an
 * inherited pipe (argv_fd) rather than encoded into env, and exec failures
 * are reported back through a second pipe (err_fd); both fds are passed to
 * the helper by number via env.
 */
static char **
pty_make_spawn_env(int slave_fd, int argv_fd, int err_fd) {
  char **current_env = *_NSGetEnviron();
  size_t env_count = 0;
  size_t keep_count = 0;
  while (current_env[env_count] != NULL) {
    if (!pty_is_helper_env_name(current_env[env_count])) {
      keep_count += 1;
    }
    env_count += 1;
  }

  /* Slots needed: kept-from-parent + 4 helper vars + terminator */
  char **child_env = (char **)calloc(keep_count + 5, sizeof(char *));
  if (!child_env) {
    return NULL;
  }

  size_t dst = 0;
  for (size_t i = 0; i < env_count; i++) {
    if (!pty_is_helper_env_name(current_env[i])) {
      child_env[dst++] = current_env[i];
    }
  }

  char *helper_mode = strdup(MOONBIT_PTY_EXEC_ENV "=1");
  char *slave_env = (char *)malloc(64);
  char *argv_env = (char *)malloc(64);
  char *err_env = (char *)malloc(64);
  if (!helper_mode || !slave_env || !argv_env || !err_env) {
    free(helper_mode);
    free(slave_env);
    free(argv_env);
    free(err_env);
    free(child_env);
    return NULL;
  }

  snprintf(slave_env, 64, MOONBIT_PTY_SLAVE_FD_ENV "=%d", slave_fd);
  snprintf(argv_env, 64, MOONBIT_PTY_ARGV_FD_ENV "=%d", argv_fd);
  snprintf(err_env, 64, MOONBIT_PTY_ERR_FD_ENV "=%d", err_fd);

  child_env[dst++] = helper_mode;
  child_env[dst++] = slave_env;
  child_env[dst++] = argv_env;
  child_env[dst++] = err_env;
  child_env[dst] = NULL;
  return child_env;
}

static void
pty_free_spawn_env(char **child_env) {
  if (!child_env) {
    return;
  }
  size_t i = 0;
  while (child_env[i] != NULL) {
    if (pty_is_helper_env_name(child_env[i])) {
      free(child_env[i]);
    }
    i += 1;
  }
  free(child_env);
}

static MoonBitPty *
pty_spawn_via_self_helper(
  const uint8_t *argv_flat,
  int32_t argc,
  int32_t cols,
  int32_t rows
) {
  if (argc <= 0 || !argv_flat) {
    return pty_make_failure((int32_t)EINVAL);
  }
  int32_t flat_len = (int32_t)Moonbit_array_length(argv_flat);
  if (flat_len <= 0) {
    return pty_make_failure((int32_t)EINVAL);
  }

  char *self_path = pty_get_self_executable_path();
  if (!self_path) {
    return pty_make_failure((int32_t)(errno ? errno : ENOMEM));
  }

  struct winsize ws;
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = (unsigned short)cols;
  ws.ws_row = (unsigned short)rows;

  int master_fd = -1;
  int slave_fd = -1;
  if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) < 0) {
    int32_t saved = (int32_t)errno;
    free(self_path);
    return pty_make_failure(saved);
  }
  pty_set_nonblocking(master_fd);

  /* argv_pipe: parent writes the flattened argv, helper reads it in the
   * constructor. err_pipe: helper writes errno on any pre-exec failure;
   * a successful execvp auto-closes the write end via FD_CLOEXEC, which
   * gives the parent an EOF = success signal. */
  int argv_pipe[2] = { -1, -1 };
  int err_pipe[2] = { -1, -1 };

  if (pipe(argv_pipe) < 0) {
    int32_t saved = (int32_t)errno;
    close(master_fd);
    close(slave_fd);
    free(self_path);
    return pty_make_failure(saved);
  }
  if (pipe(err_pipe) < 0) {
    int32_t saved = (int32_t)errno;
    close(argv_pipe[0]);
    close(argv_pipe[1]);
    close(master_fd);
    close(slave_fd);
    free(self_path);
    return pty_make_failure(saved);
  }
  /* The helper sets FD_CLOEXEC on its copy of err_pipe[1] just before
   * execvp, so we don't need to do it here. (addinherit_np under
   * POSIX_SPAWN_CLOEXEC_DEFAULT clears FD_CLOEXEC on inherited fds
   * anyway.) */

  posix_spawn_file_actions_t file_actions;
  posix_spawnattr_t attr;
  int spawn_err = posix_spawn_file_actions_init(&file_actions);
  if (spawn_err != 0) {
    close(argv_pipe[0]);
    close(argv_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    close(master_fd);
    close(slave_fd);
    free(self_path);
    return pty_make_failure((int32_t)spawn_err);
  }
  spawn_err = posix_spawnattr_init(&attr);
  if (spawn_err != 0) {
    posix_spawn_file_actions_destroy(&file_actions);
    close(argv_pipe[0]);
    close(argv_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    close(master_fd);
    close(slave_fd);
    free(self_path);
    return pty_make_failure((int32_t)spawn_err);
  }

  spawn_err = posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
  if (spawn_err == 0)
    spawn_err = posix_spawn_file_actions_addinherit_np(&file_actions, slave_fd);
  if (spawn_err == 0)
    spawn_err =
      posix_spawn_file_actions_addinherit_np(&file_actions, argv_pipe[0]);
  if (spawn_err == 0)
    spawn_err =
      posix_spawn_file_actions_addinherit_np(&file_actions, err_pipe[1]);

  char **spawn_argv = pty_make_spawn_argv(self_path);
  char **child_env = pty_make_spawn_env(slave_fd, argv_pipe[0], err_pipe[1]);
  if (!spawn_argv || !child_env) {
    spawn_err = ENOMEM;
  }

  pid_t pid = -1;
  if (spawn_err == 0) {
    spawn_err =
      posix_spawn(&pid, self_path, &file_actions, &attr, spawn_argv, child_env);
  }

  posix_spawn_file_actions_destroy(&file_actions);
  posix_spawnattr_destroy(&attr);
  free(spawn_argv);
  pty_free_spawn_env(child_env);
  free(self_path);

  /* Parent keeps only its own ends: argv_pipe[1] to write, err_pipe[0]
   * to read. Closing err_pipe[1] here is what lets the parent's read()
   * reach EOF once the helper's copy closes (either via FD_CLOEXEC on
   * successful exec, or via process exit). */
  close(slave_fd);
  close(argv_pipe[0]);
  close(err_pipe[1]);

  if (spawn_err != 0) {
    close(argv_pipe[1]);
    close(err_pipe[0]);
    close(master_fd);
    return pty_make_failure((int32_t)spawn_err);
  }

  /* Stream argv to the helper. EPIPE (helper died before reading) isn't
   * fatal here — we'll surface the real reason from the error pipe. */
  (void)pty_write_all(argv_pipe[1], argv_flat, (size_t)flat_len);
  close(argv_pipe[1]);

  int32_t child_err = 0;
  int got = pty_read_child_error(err_pipe[0], &child_err);
  close(err_pipe[0]);

  if (got == 1) {
    /* Helper reported a pre-exec failure; reap it before returning. */
    int status;
    (void)waitpid(pid, &status, 0);
    close(master_fd);
    return pty_make_failure(child_err);
  }
  if (got == 0) {
    /* EOF without errno normally means a successful execvp. But a helper
     * killed before it could write would also look like this, so do a
     * non-blocking waitpid to make sure we're not handing back a PID
     * that's already gone. */
    int status;
    pid_t ret = waitpid(pid, &status, WNOHANG);
    if (ret == pid) {
      close(master_fd);
      return pty_make_failure((int32_t)EIO);
    }
  }

  pty_handle_t *h = (pty_handle_t *)calloc(1, sizeof(pty_handle_t));
  if (!h) {
    close(master_fd);
    kill(pid, SIGHUP);
    return pty_make_failure((int32_t)ENOMEM);
  }
  h->master_fd = master_fd;
  h->spawned_pid = (int)pid;
  h->child_pid = (int)pid;

  return pty_make_success(h);
}
#endif

/* ---- platform close ----------------------------------------------------- */

static void
pty_refresh_child_status(pty_handle_t *h) {
  if (!h || h->child_exited || h->child_pid <= 0) {
    return;
  }

  int status = 0;
  pid_t ret = waitpid(h->child_pid, &status, WNOHANG);
  if (ret == h->child_pid) {
    h->child_exited = 1;
    h->child_status = status;
    h->child_pid = -1;
  }
}

static void
pty_close_impl(pty_handle_t *h) {
  pty_refresh_child_status(h);
  if (h->master_fd >= 0) {
    close(h->master_fd);
    h->master_fd = -1;
  }
  if (h->child_pid > 0) {
    kill(h->child_pid, SIGHUP);
    int status;
    waitpid(h->child_pid, &status, 0);
    h->child_pid = -1;
  }
}

/* ---- spawn -------------------------------------------------------------- */

MOONBIT_FFI_EXPORT
MoonBitPty *
moonbit_pty_spawn(
  const uint8_t *argv_flat,
  int32_t argc,
  int32_t cols,
  int32_t rows
) {
#if defined(__APPLE__)
  return pty_spawn_via_self_helper(argv_flat, argc, cols, rows);
#else
  /* Parse argv in the parent BEFORE fork so allocation failures are
   * recoverable; the child inherits the parsed memory via COW. */
  char **child_argv = pty_parse_argv_flat(argv_flat, argc);
  if (!child_argv)
    return pty_make_failure((int32_t)(errno ? errno : ENOMEM));

  /* CLOEXEC error pipe: the child writes its errno to this pipe on any
   * pre-exec failure, and a successful execvp auto-closes the write end
   * so the parent sees EOF = success. pipe2(O_CLOEXEC) is atomic on
   * Linux/FreeBSD, avoiding a race where a concurrent fork in another
   * thread could inherit the fd. */
  int err_pipe[2] = { -1, -1 };
  if (pipe2(err_pipe, O_CLOEXEC) < 0) {
    int32_t saved = (int32_t)errno;
    pty_free_argv(child_argv);
    return pty_make_failure(saved);
  }

  struct winsize ws;
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = (unsigned short)cols;
  ws.ws_row = (unsigned short)rows;

  int master_fd = -1;
  pid_t pid = forkpty(&master_fd, NULL, NULL, &ws);

  if (pid < 0) {
    /* forkpty failed */
    int32_t saved = (int32_t)errno;
    close(err_pipe[0]);
    close(err_pipe[1]);
    pty_free_argv(child_argv);
    return pty_make_failure(saved);
  }

  if (pid == 0) {
    /* ---- child process ---- */
    close(err_pipe[0]);

    /* Reset signal mask — the MoonBit async runtime blocks SIGCHLD
       and ignores SIGPIPE; the child inherits both across fork.
       We must restore defaults before exec so the shell works. */
    sigset_t all_signals;
    sigfillset(&all_signals);
    sigprocmask(SIG_UNBLOCK, &all_signals, NULL);
    signal(SIGPIPE, SIG_DFL);

    /* PATH lookup via execvp so `tun-server nvim` resolves via $PATH.
     * err_pipe[1] has O_CLOEXEC, so success auto-closes it for us. */
    execvp(child_argv[0], child_argv);
    /* exec failed — report the errno to the parent. */
    pty_report_error(err_pipe[1], errno);
    _exit(127);
  }

  /* ---- parent process ---- */
  pty_free_argv(child_argv);
  close(err_pipe[1]);

  int32_t child_err = 0;
  int got = pty_read_child_error(err_pipe[0], &child_err);
  close(err_pipe[0]);

  if (got == 1) {
    int status;
    (void)waitpid(pid, &status, 0);
    close(master_fd);
    return pty_make_failure(child_err);
  }

  /* Set master fd to non-blocking for async reads. */
  pty_set_nonblocking(master_fd);

  /* Allocate the C-side handle. */
  pty_handle_t *h = (pty_handle_t *)calloc(1, sizeof(pty_handle_t));
  if (!h) {
    close(master_fd);
    kill(pid, SIGHUP);
    return pty_make_failure((int32_t)ENOMEM);
  }
  h->master_fd = master_fd;
  h->spawned_pid = (int)pid;
  h->child_pid = (int)pid;
  h->child_exited = 0;
  h->child_status = 0;

  return pty_make_success(h);
#endif
}

/* ---- resize ------------------------------------------------------------- */

MOONBIT_FFI_EXPORT
int32_t
moonbit_pty_resize(MoonBitPty *p, int32_t cols, int32_t rows) {
  if (!p || !p->handle || p->handle->master_fd < 0)
    return (int32_t)EINVAL;

  struct winsize ws;
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = (unsigned short)cols;
  ws.ws_row = (unsigned short)rows;

  if (ioctl(p->handle->master_fd, TIOCSWINSZ, &ws) == 0)
    return 0;
  return (int32_t)errno;
}

/* ---- read_fd ------------------------------------------------------------ */

MOONBIT_FFI_EXPORT
int32_t
moonbit_pty_take_read_fd(MoonBitPty *p) {
  if (!p || !p->handle)
    return -1;
  int fd = p->handle->master_fd;
  p->handle->master_fd = -1;
  return (int32_t)fd;
}

MOONBIT_FFI_EXPORT
int32_t
moonbit_pty_take_write_fd(MoonBitPty *p) {
  if (!p || !p->handle)
    return -1;
  int fd = p->handle->master_fd;
  p->handle->master_fd = -1;
  return (int32_t)fd;
}

MOONBIT_FFI_EXPORT
int32_t
moonbit_pty_child_pid(MoonBitPty *p) {
  if (!p || !p->handle)
    return -1;
  return (int32_t)p->handle->spawned_pid;
}

/* ---- close -------------------------------------------------------------- */

MOONBIT_FFI_EXPORT
void
moonbit_pty_close(MoonBitPty *p) {
  if (!p || !p->handle)
    return;
  pty_close_impl(p->handle);
  free(p->handle);
  p->handle = NULL;
}

/* ========================================================================== */
/*  WINDOWS IMPLEMENTATION (ConPTY)                                           */
/* ========================================================================== */
#else /* _WIN32 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* ---- ConPTY function pointer types (dynamically loaded) ----------------- */

typedef struct {
  short X;
  short Y;
} COORD_T;

typedef HRESULT(WINAPI *PFN_CreatePseudoConsole)(
  COORD_T size,
  HANDLE hInput,
  HANDLE hOutput,
  DWORD dwFlags,
  void **phPC
);
typedef HRESULT(WINAPI *PFN_ResizePseudoConsole)(void *hPC, COORD_T size);
typedef void(WINAPI *PFN_ClosePseudoConsole)(void *hPC);

static PFN_CreatePseudoConsole pfnCreatePseudoConsole = NULL;
static PFN_ResizePseudoConsole pfnResizePseudoConsole = NULL;
static PFN_ClosePseudoConsole pfnClosePseudoConsole = NULL;
static int conpty_loaded = 0;

static int
ensure_conpty(void) {
  if (conpty_loaded)
    return (pfnCreatePseudoConsole != NULL) ? 0 : -1;
  conpty_loaded = 1;

  HMODULE k32 = GetModuleHandleA("kernel32.dll");
  if (!k32)
    return -1;

  pfnCreatePseudoConsole =
    (PFN_CreatePseudoConsole)GetProcAddress(k32, "CreatePseudoConsole");
  pfnResizePseudoConsole =
    (PFN_ResizePseudoConsole)GetProcAddress(k32, "ResizePseudoConsole");
  pfnClosePseudoConsole =
    (PFN_ClosePseudoConsole)GetProcAddress(k32, "ClosePseudoConsole");

  return (pfnCreatePseudoConsole && pfnResizePseudoConsole &&
          pfnClosePseudoConsole)
           ? 0
           : -1;
}

/* ---- overlapped named-pipe helper --------------------------------------- */

static volatile LONG pty_pipe_id = 0;

static int
create_overlapped_pipe(HANDLE *read_end, HANDLE *write_end) {
  LONG id = InterlockedIncrement(&pty_pipe_id);
  char name[128];
  snprintf(
    name, sizeof(name), "\\\\.\\pipe\\moonbit_pty.%lu.%ld",
    (unsigned long)GetCurrentProcessId(), id
  );

  *write_end = CreateNamedPipeA(
    name, PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL
  );
  if (*write_end == INVALID_HANDLE_VALUE)
    return -1;

  *read_end = CreateFileA(
    name, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL
  );
  if (*read_end == INVALID_HANDLE_VALUE) {
    CloseHandle(*write_end);
    *write_end = INVALID_HANDLE_VALUE;
    return -1;
  }
  return 0;
}

/* ---- platform close ----------------------------------------------------- */

static void
pty_close_impl(pty_handle_t *h) {
  if (h->proc_handle && h->proc_handle != INVALID_HANDLE_VALUE) {
    TerminateProcess(h->proc_handle, 0);
    CloseHandle(h->proc_handle);
    h->proc_handle = NULL;
  }
  if (h->thread_handle && h->thread_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(h->thread_handle);
    h->thread_handle = NULL;
  }
  if (h->hpc) {
    pfnClosePseudoConsole(h->hpc);
    h->hpc = NULL;
  }
  if (h->pipe_in_read && h->pipe_in_read != INVALID_HANDLE_VALUE) {
    CloseHandle(h->pipe_in_read);
    h->pipe_in_read = NULL;
  }
  if (h->pipe_in_write && h->pipe_in_write != INVALID_HANDLE_VALUE) {
    CloseHandle(h->pipe_in_write);
    h->pipe_in_write = NULL;
  }
  if (h->pipe_out_read && h->pipe_out_read != INVALID_HANDLE_VALUE) {
    CloseHandle(h->pipe_out_read);
    h->pipe_out_read = NULL;
  }
  if (h->pipe_out_write && h->pipe_out_write != INVALID_HANDLE_VALUE) {
    CloseHandle(h->pipe_out_write);
    h->pipe_out_write = NULL;
  }
}

/*
 * Join a parsed argv into a single Windows command-line string.
 *
 * TODO(windows): proper CommandLineToArgvW-compatible quoting. For now this
 * does a naive space-join which works for args that don't contain spaces,
 * tabs, quotes, or backslash-quote sequences. The design doc accepts this
 * as a v1 shortcut. When Windows support becomes real, replace with full
 * escaping per
 * https://learn.microsoft.com/en-us/cpp/cpp/main-function-command-line-args#parsing-c-command-line-arguments
 */
static char *
pty_join_argv_windows(char **argv, int argc) {
  if (argc <= 0)
    return NULL;
  size_t total = 0;
  for (int i = 0; i < argc; i++) {
    total += strlen(argv[i]) + 1; /* +1 for space or terminator */
  }
  char *out = (char *)malloc(total);
  if (!out)
    return NULL;
  size_t pos = 0;
  for (int i = 0; i < argc; i++) {
    size_t len = strlen(argv[i]);
    if (i > 0) {
      out[pos++] = ' ';
    }
    memcpy(out + pos, argv[i], len);
    pos += len;
  }
  out[pos] = '\0';
  return out;
}

/* ---- spawn -------------------------------------------------------------- */

MOONBIT_FFI_EXPORT
MoonBitPty *
moonbit_pty_spawn(
  const uint8_t *argv_flat,
  int32_t argc,
  int32_t cols,
  int32_t rows
) {
  int32_t saved_err = 0;
  if (ensure_conpty() < 0) {
    /* ConPTY unavailable — no meaningful GetLastError, use a sentinel. */
    return pty_make_failure((int32_t)ERROR_NOT_SUPPORTED);
  }

  char **parsed_argv = pty_parse_argv_flat(argv_flat, argc);
  if (!parsed_argv)
    return pty_make_failure((int32_t)ERROR_NOT_ENOUGH_MEMORY);
  char *cmd_line = pty_join_argv_windows(parsed_argv, argc);
  pty_free_argv(parsed_argv);
  if (!cmd_line)
    return pty_make_failure((int32_t)ERROR_NOT_ENOUGH_MEMORY);

  HANDLE pipe_in_read = INVALID_HANDLE_VALUE;
  HANDLE pipe_in_write = INVALID_HANDLE_VALUE;
  HANDLE pipe_out_read = INVALID_HANDLE_VALUE;
  HANDLE pipe_out_write = INVALID_HANDLE_VALUE;

  /* pipe_in: keyboard → ConPTY stdin. Use overlapped named pipe so the
   * write end can be registered with IOCP for async writes from MoonBit. */
  if (create_overlapped_pipe(&pipe_in_read, &pipe_in_write) < 0) {
    saved_err = (int32_t)GetLastError();
    goto fail;
  }

  /* pipe_out: ConPTY stdout → our async reader. Use overlapped named pipe
   * so the read end can be registered with IOCP for event-driven reads. */
  if (create_overlapped_pipe(&pipe_out_read, &pipe_out_write) < 0) {
    saved_err = (int32_t)GetLastError();
    goto fail;
  }

  /* Create the pseudo-console. */
  COORD_T size;
  size.X = (short)cols;
  size.Y = (short)rows;

  void *hpc = NULL;
  HRESULT hr =
    pfnCreatePseudoConsole(size, pipe_in_read, pipe_out_write, 0, &hpc);
  if (FAILED(hr) || !hpc) {
    saved_err = (int32_t)hr;
    goto fail;
  }

  /* Prepare STARTUPINFOEXA with the pseudo-console attribute. */
  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
  LPPROC_THREAD_ATTRIBUTE_LIST attr_list =
    (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attr_size);
  if (!attr_list) {
    saved_err = (int32_t)ERROR_NOT_ENOUGH_MEMORY;
    goto fail_close_hpc;
  }
  if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size)) {
    saved_err = (int32_t)GetLastError();
    HeapFree(GetProcessHeap(), 0, attr_list);
    goto fail_close_hpc;
  }

  /* PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = 0x00020016 */
  if (!UpdateProcThreadAttribute(
        attr_list, 0,
        (DWORD_PTR)0x00020016, /* PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE */
        hpc, sizeof(void *), NULL, NULL
      )) {
    saved_err = (int32_t)GetLastError();
    DeleteProcThreadAttributeList(attr_list);
    HeapFree(GetProcessHeap(), 0, attr_list);
    goto fail_close_hpc;
  }

  STARTUPINFOEXA si;
  ZeroMemory(&si, sizeof(si));
  si.StartupInfo.cb = sizeof(STARTUPINFOEXA);
  si.lpAttributeList = attr_list;

  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  BOOL ok = CreateProcessA(
    NULL, cmd_line, /* command line (mutable copy OK — Windows makes its own) */
    NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
    &si.StartupInfo, &pi
  );

  DeleteProcThreadAttributeList(attr_list);
  HeapFree(GetProcessHeap(), 0, attr_list);

  /* CreateProcessA has consumed the command line; safe to free now. */
  free(cmd_line);
  cmd_line = NULL;

  if (!ok) {
    saved_err = (int32_t)GetLastError();
    goto fail_close_hpc;
  }

  /* Build the handle. */
  pty_handle_t *h = (pty_handle_t *)calloc(1, sizeof(pty_handle_t));
  if (!h) {
    saved_err = (int32_t)ERROR_NOT_ENOUGH_MEMORY;
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    goto fail_close_hpc;
  }
  h->hpc = hpc;
  h->pipe_in_read = pipe_in_read;
  h->pipe_in_write = pipe_in_write;
  h->pipe_out_read = pipe_out_read;
  h->pipe_out_write = pipe_out_write;
  h->proc_handle = pi.hProcess;
  h->thread_handle = pi.hThread;

  return pty_make_success(h);

fail_close_hpc:
  pfnClosePseudoConsole(hpc);
fail:
  if (pipe_in_read != INVALID_HANDLE_VALUE)
    CloseHandle(pipe_in_read);
  if (pipe_in_write != INVALID_HANDLE_VALUE)
    CloseHandle(pipe_in_write);
  if (pipe_out_read != INVALID_HANDLE_VALUE)
    CloseHandle(pipe_out_read);
  if (pipe_out_write != INVALID_HANDLE_VALUE)
    CloseHandle(pipe_out_write);
  free(cmd_line);
  return pty_make_failure(saved_err);
}

/* ---- resize ------------------------------------------------------------- */

MOONBIT_FFI_EXPORT
int32_t
moonbit_pty_resize(MoonBitPty *p, int32_t cols, int32_t rows) {
  if (!p || !p->handle || !p->handle->hpc)
    return (int32_t)ERROR_INVALID_PARAMETER;

  COORD_T size;
  size.X = (short)cols;
  size.Y = (short)rows;
  HRESULT hr = pfnResizePseudoConsole(p->handle->hpc, size);
  return SUCCEEDED(hr) ? 0 : (int32_t)hr;
}

/* ---- read_fd ------------------------------------------------------------ */

MOONBIT_FFI_EXPORT
HANDLE
moonbit_pty_take_read_fd(MoonBitPty *p) {
  if (!p || !p->handle)
    return INVALID_HANDLE_VALUE;
  HANDLE fd = p->handle->pipe_out_read;
  p->handle->pipe_out_read = INVALID_HANDLE_VALUE;
  return fd;
}

MOONBIT_FFI_EXPORT
HANDLE
moonbit_pty_take_write_fd(MoonBitPty *p) {
  if (!p || !p->handle)
    return INVALID_HANDLE_VALUE;
  HANDLE fd = p->handle->pipe_in_write;
  p->handle->pipe_in_write = INVALID_HANDLE_VALUE;
  return fd;
}

MOONBIT_FFI_EXPORT
int32_t
moonbit_pty_child_pid(MoonBitPty *p) {
  if (!p || !p->handle || !p->handle->proc_handle)
    return -1;
  return (int32_t)GetProcessId(p->handle->proc_handle);
}

/* ---- close -------------------------------------------------------------- */

MOONBIT_FFI_EXPORT
void
moonbit_pty_close(MoonBitPty *p) {
  if (!p || !p->handle)
    return;
  pty_close_impl(p->handle);
  free(p->handle);
  p->handle = NULL;
}

#endif /* _WIN32 */
