/* SPDX-License-Identifier: Apache-2.0
 * The svrtest driver's companion application (Windows): connects to the driver's named pipe, as a body estimator's
 * application does, and sends 30 waist positions "x y z\n" moving along x, 100 ms apart. Exit 0 once all were sent,
 * 1 when the pipe cannot be opened (the driver is not in this wineserver). */
#include <stdio.h>
#include <windows.h>

int main(void) {
  HANDLE p = INVALID_HANDLE_VALUE;
  for (int i = 0; i < 50 && p == INVALID_HANDLE_VALUE; i++) {
    p = CreateFileA("\\\\.\\pipe\\svrtest", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (p == INVALID_HANDLE_VALUE) Sleep(100);
  }
  if (p == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "companion: pipe not found (%lu)\n", (unsigned long)GetLastError());
    return 1;
  }
  for (int i = 0; i < 30; i++) {
    char line[64];
    int n = snprintf(line, sizeof line, "%.3f 1.000 -0.500\n", 0.1 * i);
    DWORD w;
    if (!WriteFile(p, line, (DWORD)n, &w, NULL)) return 1;
    Sleep(100);
  }
  fprintf(stderr, "companion: sent 30 positions\n");
  CloseHandle(p);
  return 0;
}
