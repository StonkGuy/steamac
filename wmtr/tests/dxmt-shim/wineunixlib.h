/* Test stand-in for Wine's wineunixlib.h: DXMT's guest-side thunks (winemetal_thunks.c) call the
 * wmtr client's __wine_unix_call_funcs directly instead of Wine's unix call dispatcher. */
#ifndef WMTR_TEST_WINEUNIXLIB_H
#define WMTR_TEST_WINEUNIXLIB_H
typedef int NTSTATUS;
NTSTATUS wmtr_test_unix_call(unsigned int code, void *args);
#define WINE_UNIX_CALL(code, args) wmtr_test_unix_call((code), (args))
#endif
