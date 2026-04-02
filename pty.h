#ifndef MOONBIT_PTY_H
#define MOONBIT_PTY_H

#include <moonbit.h>

typedef struct moonbit_pty *moonbit_pty_t;

MOONBIT_FFI_EXPORT
moonbit_pty_t
moonbit_pty_spawn(moonbit_bytes_t shell, int32_t cols, int32_t rows);

MOONBIT_FFI_EXPORT
int32_t
moonbit_pty_read(
  moonbit_pty_t pty,
  moonbit_bytes_t buffer,
  int32_t offset,
  int32_t length
);

MOONBIT_FFI_EXPORT
int32_t
moonbit_pty_write(
  moonbit_pty_t pty,
  moonbit_bytes_t buffer,
  int32_t offset,
  int32_t length
);

MOONBIT_FFI_EXPORT
int32_t
moonbit_pty_resize(moonbit_pty_t pty, int32_t cols, int32_t rows);

MOONBIT_FFI_EXPORT
void
moonbit_pty_close(moonbit_pty_t pty);

#endif // MOONBIT_PTY_H
