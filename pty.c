/*
 * pty.c -- Cross-platform PTY for MoonBit FFI.
 *
 * Unix:    forkpty() + fcntl(O_NONBLOCK) + ioctl(TIOCSWINSZ)
 * Windows: ConPTY (dynamically loaded from kernel32.dll)
 */

#include "pty.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/*  Handle layout                                                         */
/* ---------------------------------------------------------------------- */

struct moonbit_pty {
#ifdef _WIN32
  void *hpc;           /* HPCON                          */
  void *pipe_in_write; /* write end  -> child stdin      */
  void *pipe_out_read; /* read  end  <- child stdout     */
  void *proc_handle;   /* child PROCESS_INFORMATION      */
  void *thread_handle; /* child thread                   */
#else
  int master_fd;
  int child_pid;
#endif
};

/* ====================================================================== */
/*  UNIX                                                                  */
/* ====================================================================== */
#ifndef _WIN32

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__)
#include <util.h>
#else /* Linux and others */
#include <pty.h>
#endif

/*
 * Close all file descriptors above stderr in the child process.
 * Prevents leaking fds from the parent (e.g. macOS Cocoa, gnome-shell).
 */
static void
close_fds_above_stderr(void) {
  DIR *dir = opendir("/dev/fd");
  if (!dir)
    dir = opendir("/proc/self/fd");
  if (!dir)
    return;
  int dfd = dirfd(dir);
  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    char *end;
    long fd = strtol(ent->d_name, &end, 10);
    if (*end != '\0' || fd < 0)
      continue;
    if (fd > 2 && fd != dfd)
      close((int)fd);
  }
  closedir(dir);
}

/* ---- spawn ----------------------------------------------------------- */

MOONBIT_FFI_EXPORT
moonbit_pty_t
moonbit_pty_spawn(moonbit_bytes_t shell, int32_t cols, int32_t rows) {
  struct winsize ws;
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = (unsigned short)cols;
  ws.ws_row = (unsigned short)rows;

  /* Sensible terminal defaults. */
  struct termios term;
  memset(&term, 0, sizeof(term));
  term.c_iflag = ICRNL | IXON | IXANY | IMAXBEL | BRKINT;
#if defined(IUTF8)
  term.c_iflag |= IUTF8;
#endif
  term.c_oflag = OPOST | ONLCR;
  term.c_cflag = CREAD | CS8 | HUPCL;
  term.c_lflag =
    ICANON | ISIG | IEXTEN | ECHO | ECHOE | ECHOK | ECHOKE | ECHOCTL;
  term.c_cc[VEOF] = 4;      /* Ctrl-D */
  term.c_cc[VERASE] = 0x7f; /* DEL    */
  term.c_cc[VWERASE] = 23;  /* Ctrl-W */
  term.c_cc[VKILL] = 21;    /* Ctrl-U */
  term.c_cc[VINTR] = 3;     /* Ctrl-C */
  term.c_cc[VQUIT] = 0x1c;  /* Ctrl-\ */
  term.c_cc[VSUSP] = 26;    /* Ctrl-Z */
  term.c_cc[VSTART] = 17;   /* Ctrl-Q */
  term.c_cc[VSTOP] = 19;    /* Ctrl-S */
  cfsetispeed(&term, B38400);
  cfsetospeed(&term, B38400);

  int master_fd = -1;
  pid_t pid;

  /* Block signals during fork to prevent race conditions. */
  sigset_t all_sigs, old_sigs;
  sigfillset(&all_sigs);
  pthread_sigmask(SIG_SETMASK, &all_sigs, &old_sigs);

  pid = forkpty(&master_fd, NULL, &term, &ws);

  if (pid < 0) {
    pthread_sigmask(SIG_SETMASK, &old_sigs, NULL);
    return NULL;
  }

  if (pid == 0) {
    /* === Child === */

    /* Reset all signal handlers to default. */
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    for (int i = 1; i < NSIG; i++)
      sigaction(i, &sa, NULL);

    /* Unblock all signals. */
    sigset_t empty;
    sigemptyset(&empty);
    pthread_sigmask(SIG_SETMASK, &empty, NULL);

    /* Close leaked fds from parent (macOS Cocoa, gnome-shell, etc.). */
    close_fds_above_stderr();

    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);

    execlp((const char *)shell, (const char *)shell, (char *)NULL);
    _exit(127);
  }

  /* === Parent === */
  pthread_sigmask(SIG_SETMASK, &old_sigs, NULL);

  /* Non-blocking for async reads. */
  int flags = fcntl(master_fd, F_GETFL, 0);
  if (flags >= 0)
    fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

  /* Prevent master fd from leaking into future children. */
  int fd_flags = fcntl(master_fd, F_GETFD, 0);
  if (fd_flags >= 0)
    fcntl(master_fd, F_SETFD, fd_flags | FD_CLOEXEC);

  struct moonbit_pty *p =
    (struct moonbit_pty *)moonbit_make_bytes(sizeof(struct moonbit_pty), 0);
  p->master_fd = master_fd;
  p->child_pid = (int)pid;
  return p;
}

/* ---- read (non-blocking) --------------------------------------------- */

MOONBIT_FFI_EXPORT
int32_t
moonbit_pty_read(
  moonbit_pty_t pty,
  moonbit_bytes_t buffer,
  int32_t offset,
  int32_t length
) {
  if (!pty || pty->master_fd < 0)
    return -1;

  ssize_t n = read(pty->master_fd, buffer + offset, (size_t)length);
  if (n > 0)
    return (int32_t)n;
  if (n == 0)
    return -1; /* EOF */
  if (errno == EAGAIN || errno == EWOULDBLOCK)
    return 0; /* would block */
  if (errno == EIO)
    return -1; /* slave closed */
  return -1;
}

/* ---- write (blocking) ------------------------------------------------ */

MOONBIT_FFI_EXPORT
int32_t
moonbit_pty_write(
  moonbit_pty_t pty,
  moonbit_bytes_t buffer,
  int32_t offset,
  int32_t length
) {
  if (!pty || pty->master_fd < 0)
    return -1;

  int fd = pty->master_fd;

  /* Temporarily switch to blocking for a reliable write. */
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0 && (flags & O_NONBLOCK))
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

  ssize_t n = write(fd, buffer + offset, (size_t)length);

  /* Restore non-blocking. */
  if (flags >= 0 && (flags & O_NONBLOCK))
    fcntl(fd, F_SETFL, flags);

  return (n >= 0) ? (int32_t)n : -1;
}

/* ---- resize ---------------------------------------------------------- */

MOONBIT_FFI_EXPORT
int32_t
moonbit_pty_resize(moonbit_pty_t pty, int32_t cols, int32_t rows) {
  if (!pty || pty->master_fd < 0)
    return -1;

  struct winsize ws;
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = (unsigned short)cols;
  ws.ws_row = (unsigned short)rows;
  return (ioctl(pty->master_fd, TIOCSWINSZ, &ws) == 0) ? 0 : -1;
}

/* ---- close ----------------------------------------------------------- */

MOONBIT_FFI_EXPORT
void
moonbit_pty_close(moonbit_pty_t pty) {
  if (!pty)
    return;
  if (pty->master_fd >= 0) {
    close(pty->master_fd);
    pty->master_fd = -1;
  }
  if (pty->child_pid > 0) {
    kill(pty->child_pid, SIGHUP);
    int status;
    waitpid(pty->child_pid, &status, 0);
    pty->child_pid = -1;
  }
}

/* ====================================================================== */
/*  WINDOWS  (ConPTY, dynamically loaded)                                 */
/* ====================================================================== */
#else /* _WIN32 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* ---- ConPTY function pointers ---------------------------------------- */

typedef HRESULT(WINAPI *PFN_CreatePseudoConsole)(
  COORD size,
  HANDLE hInput,
  HANDLE hOutput,
  DWORD dwFlags,
  void **phPC
);
typedef HRESULT(WINAPI *PFN_ResizePseudoConsole)(void *hPC, COORD size);
typedef void(WINAPI *PFN_ClosePseudoConsole)(void *hPC);

static PFN_CreatePseudoConsole pfn_create;
static PFN_ResizePseudoConsole pfn_resize;
static PFN_ClosePseudoConsole pfn_close;
static int conpty_loaded;

static int
ensure_conpty(void) {
  if (conpty_loaded)
    return pfn_create ? 0 : -1;
  conpty_loaded = 1;

  HMODULE k32 = GetModuleHandleA("kernel32.dll");
  if (k32) {
    pfn_create =
      (PFN_CreatePseudoConsole)GetProcAddress(k32, "CreatePseudoConsole");
    pfn_resize =
      (PFN_ResizePseudoConsole)GetProcAddress(k32, "ResizePseudoConsole");
    pfn_close =
      (PFN_ClosePseudoConsole)GetProcAddress(k32, "ClosePseudoConsole");
  }

  /* Fallback: try sideloaded conpty.dll */
  if (!pfn_create) {
    HMODULE conpty = LoadLibraryA("conpty.dll");
    if (conpty) {
      pfn_create =
        (PFN_CreatePseudoConsole)GetProcAddress(conpty, "CreatePseudoConsole");
      pfn_resize =
        (PFN_ResizePseudoConsole)GetProcAddress(conpty, "ResizePseudoConsole");
      pfn_close =
        (PFN_ClosePseudoConsole)GetProcAddress(conpty, "ClosePseudoConsole");
    }
  }

  return (pfn_create && pfn_resize && pfn_close) ? 0 : -1;
}

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE                                    \
  ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)
#endif

#ifndef PSEUDOCONSOLE_INHERIT_CURSOR
#define PSEUDOCONSOLE_INHERIT_CURSOR 0x1
#endif
#ifndef PSEUDOCONSOLE_RESIZE_QUIRK
#define PSEUDOCONSOLE_RESIZE_QUIRK 0x2
#endif
#ifndef PSEUDOCONSOLE_WIN32_INPUT_MODE
#define PSEUDOCONSOLE_WIN32_INPUT_MODE 0x4
#endif

/* ---- spawn ----------------------------------------------------------- */

MOONBIT_FFI_EXPORT
moonbit_pty_t
moonbit_pty_spawn(moonbit_bytes_t shell, int32_t cols, int32_t rows) {
  if (ensure_conpty() < 0)
    return NULL;

  HANDLE pipe_in_read = INVALID_HANDLE_VALUE;
  HANDLE pipe_in_write = INVALID_HANDLE_VALUE;
  HANDLE pipe_out_read = INVALID_HANDLE_VALUE;
  HANDLE pipe_out_write = INVALID_HANDLE_VALUE;

  if (!CreatePipe(&pipe_in_read, &pipe_in_write, NULL, 0))
    return NULL;
  if (!CreatePipe(&pipe_out_read, &pipe_out_write, NULL, 0))
    goto fail_pipes;

  /* Create pseudo-console. */
  COORD size;
  size.X = (short)cols;
  size.Y = (short)rows;

  DWORD conpty_flags = PSEUDOCONSOLE_INHERIT_CURSOR
                       | PSEUDOCONSOLE_RESIZE_QUIRK
                       | PSEUDOCONSOLE_WIN32_INPUT_MODE;

  void *hpc = NULL;
  HRESULT hr = pfn_create(size, pipe_in_read, pipe_out_write, conpty_flags, &hpc);
  if (FAILED(hr) || !hpc)
    goto fail_pipes;

  /* Close pipe ends now owned by ConPTY. */
  CloseHandle(pipe_in_read);
  pipe_in_read = INVALID_HANDLE_VALUE;
  CloseHandle(pipe_out_write);
  pipe_out_write = INVALID_HANDLE_VALUE;

  /* Non-blocking reads on output pipe. */
  {
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    SetNamedPipeHandleState(pipe_out_read, &mode, NULL, NULL);
  }

  /* Prepare STARTUPINFOEXW with pseudo-console attribute. */
  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);

  LPPROC_THREAD_ATTRIBUTE_LIST attr_list =
    (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attr_size);
  if (!attr_list)
    goto fail_hpc;

  if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size)) {
    HeapFree(GetProcessHeap(), 0, attr_list);
    goto fail_hpc;
  }

  if (!UpdateProcThreadAttribute(
        attr_list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc, sizeof(void *),
        NULL, NULL
      )) {
    DeleteProcThreadAttributeList(attr_list);
    HeapFree(GetProcessHeap(), 0, attr_list);
    goto fail_hpc;
  }

  STARTUPINFOEXW si;
  ZeroMemory(&si, sizeof(si));
  si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
  si.lpAttributeList = attr_list;

  /* Prevent child from inheriting parent's redirected stdio. */
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
  si.StartupInfo.hStdOutput = INVALID_HANDLE_VALUE;
  si.StartupInfo.hStdError = INVALID_HANDLE_VALUE;

  /* Convert shell path from UTF-8 to wide string. */
  int shell_len = (int)strlen((const char *)shell);
  int wlen =
    MultiByteToWideChar(CP_UTF8, 0, (const char *)shell, shell_len, NULL, 0);
  WCHAR *wshell =
    (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (wlen + 1) * sizeof(WCHAR));
  if (!wshell) {
    DeleteProcThreadAttributeList(attr_list);
    HeapFree(GetProcessHeap(), 0, attr_list);
    goto fail_hpc;
  }
  MultiByteToWideChar(CP_UTF8, 0, (const char *)shell, shell_len, wshell, wlen);
  wshell[wlen] = L'\0';

  /* Launch the child process. */
  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  BOOL ok = CreateProcessW(
    NULL, wshell, NULL, NULL, FALSE,
    EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, NULL, NULL,
    &si.StartupInfo, &pi
  );

  HeapFree(GetProcessHeap(), 0, wshell);
  DeleteProcThreadAttributeList(attr_list);
  HeapFree(GetProcessHeap(), 0, attr_list);

  if (!ok)
    goto fail_hpc;

  /* Build MoonBit handle. */
  struct moonbit_pty *p =
    (struct moonbit_pty *)moonbit_make_bytes(sizeof(struct moonbit_pty), 0);
  p->hpc = hpc;
  p->pipe_in_write = pipe_in_write;
  p->pipe_out_read = pipe_out_read;
  p->proc_handle = pi.hProcess;
  p->thread_handle = pi.hThread;
  return p;

fail_hpc:
  pfn_close(hpc);
  CloseHandle(pipe_in_write);
  CloseHandle(pipe_out_read);
  return NULL;

fail_pipes:
  if (pipe_in_read != INVALID_HANDLE_VALUE)
    CloseHandle(pipe_in_read);
  if (pipe_in_write != INVALID_HANDLE_VALUE)
    CloseHandle(pipe_in_write);
  if (pipe_out_read != INVALID_HANDLE_VALUE)
    CloseHandle(pipe_out_read);
  if (pipe_out_write != INVALID_HANDLE_VALUE)
    CloseHandle(pipe_out_write);
  return NULL;
}

/* ---- read (non-blocking) --------------------------------------------- */

MOONBIT_FFI_EXPORT
int32_t
moonbit_pty_read(
  moonbit_pty_t pty,
  moonbit_bytes_t buffer,
  int32_t offset,
  int32_t length
) {
  if (!pty || !pty->pipe_out_read)
    return -1;

  DWORD n = 0;
  BOOL ok =
    ReadFile(pty->pipe_out_read, buffer + offset, (DWORD)length, &n, NULL);
  if (ok && n > 0)
    return (int32_t)n;
  if (!ok && GetLastError() == ERROR_NO_DATA)
    return 0; /* would block */
  return -1;
}

/* ---- write ----------------------------------------------------------- */

MOONBIT_FFI_EXPORT
int32_t
moonbit_pty_write(
  moonbit_pty_t pty,
  moonbit_bytes_t buffer,
  int32_t offset,
  int32_t length
) {
  if (!pty || !pty->pipe_in_write)
    return -1;

  DWORD n = 0;
  BOOL ok =
    WriteFile(pty->pipe_in_write, buffer + offset, (DWORD)length, &n, NULL);
  return ok ? (int32_t)n : -1;
}

/* ---- resize ---------------------------------------------------------- */

MOONBIT_FFI_EXPORT
int32_t
moonbit_pty_resize(moonbit_pty_t pty, int32_t cols, int32_t rows) {
  if (!pty || !pty->hpc)
    return -1;

  COORD size;
  size.X = (short)cols;
  size.Y = (short)rows;
  return SUCCEEDED(pfn_resize(pty->hpc, size)) ? 0 : -1;
}

/* ---- close ----------------------------------------------------------- */

MOONBIT_FFI_EXPORT
void
moonbit_pty_close(moonbit_pty_t pty) {
  if (!pty)
    return;
  if (pty->proc_handle && pty->proc_handle != INVALID_HANDLE_VALUE) {
    TerminateProcess(pty->proc_handle, 0);
    CloseHandle(pty->proc_handle);
    pty->proc_handle = NULL;
  }
  if (pty->thread_handle && pty->thread_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(pty->thread_handle);
    pty->thread_handle = NULL;
  }
  if (pty->hpc) {
    pfn_close(pty->hpc);
    pty->hpc = NULL;
  }
  if (pty->pipe_in_write && pty->pipe_in_write != INVALID_HANDLE_VALUE) {
    CloseHandle(pty->pipe_in_write);
    pty->pipe_in_write = NULL;
  }
  if (pty->pipe_out_read && pty->pipe_out_read != INVALID_HANDLE_VALUE) {
    CloseHandle(pty->pipe_out_read);
    pty->pipe_out_read = NULL;
  }
}

#endif /* _WIN32 */
