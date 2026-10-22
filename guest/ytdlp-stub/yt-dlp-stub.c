/* SPDX-License-Identifier: MIT
 * steamac: Tools/yt-dlp.exe stand-in for VRChat in a Proton prefix (steamac.ytdlp=native).
 *
 * Sends its own command line (after argv[0]) to the guest's native yt-dlp server on 127.0.0.1:47811
 * (/usr/lib/steamac/ytdlp/ytdlp_server.py) and replays what it answers: stdout, stderr, exit code.
 * When the server is not there, or has no native yt-dlp yet, it runs the original VRChat yt-dlp.exe
 * kept next to it (yt-dlp.exe.steamac-orig) with the same arguments and handles, so the video player
 * still works, only slower. No C runtime: kernel32 + ws2_32 only, a few KiB.
 */
/* The few Win32 declarations used, so it builds without Windows headers (plain clang + lld-link). */
typedef unsigned short WCHAR;
typedef unsigned int DWORD, UINT;
typedef int BOOL;
typedef void *HANDLE;
typedef unsigned long long SOCKET;
#define WINAPI __stdcall
#define NULL ((void *)0)
#define TRUE 1
#define MAX_PATH 260
#define INFINITE 0xffffffffu
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
#define STARTF_USESTDHANDLES 0x100
#define CP_UTF8 65001
#define AF_INET 2
#define SOCK_STREAM 1
#define IPPROTO_TCP 6
#define INVALID_SOCKET (~(SOCKET)0)
#define INADDR_LOOPBACK 0x7f000001u
typedef struct { DWORD cb; WCHAR *lpReserved, *lpDesktop, *lpTitle; DWORD dwX, dwY, dwXSize, dwYSize,
                 dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags; unsigned short wShowWindow, cbReserved2;
                 unsigned char *lpReserved2; HANDLE hStdInput, hStdOutput, hStdError; } STARTUPINFOW;
typedef struct { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; } PROCESS_INFORMATION;
typedef struct { unsigned short wVersion, wHighVersion; unsigned short iMaxSockets, iMaxUdpDg; char *lpVendorInfo;
                 char szDescription[257], szSystemStatus[129]; } WSADATA;
struct sockaddr { unsigned short sa_family; char sa_data[14]; };
struct sockaddr_in { unsigned short sin_family, sin_port; unsigned int sin_addr; char sin_zero[8]; };
__declspec(dllimport) HANDLE WINAPI GetStdHandle(DWORD);
__declspec(dllimport) BOOL WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) WCHAR *WINAPI GetCommandLineW(void);
__declspec(dllimport) DWORD WINAPI GetModuleFileNameW(HANDLE, WCHAR *, DWORD);
__declspec(dllimport) BOOL WINAPI CreateProcessW(const WCHAR *, WCHAR *, void *, void *, BOOL, DWORD, void *,
                                                 const WCHAR *, STARTUPINFOW *, PROCESS_INFORMATION *);
__declspec(dllimport) DWORD WINAPI WaitForSingleObject(HANDLE, DWORD);
__declspec(dllimport) BOOL WINAPI GetExitCodeProcess(HANDLE, DWORD *);
__declspec(dllimport) BOOL WINAPI CloseHandle(HANDLE);
__declspec(dllimport) int WINAPI WideCharToMultiByte(UINT, DWORD, const WCHAR *, int, char *, int, const char *, BOOL *);
__declspec(dllimport) __attribute__((noreturn)) void WINAPI ExitProcess(UINT);
__declspec(dllimport) int WINAPI WSAStartup(unsigned short, WSADATA *);
__declspec(dllimport) SOCKET WINAPI socket(int, int, int);
__declspec(dllimport) int WINAPI connect(SOCKET, const struct sockaddr *, int);
__declspec(dllimport) int WINAPI send(SOCKET, const char *, int, int);
__declspec(dllimport) int WINAPI recv(SOCKET, char *, int, int);
__declspec(dllimport) int WINAPI closesocket(SOCKET);
__declspec(dllimport) unsigned short WINAPI htons(unsigned short);
__declspec(dllimport) unsigned int WINAPI htonl(unsigned int);
#define MAKEWORD(a, b) ((unsigned short)((a) | ((b) << 8)))
#define L(s) ((const WCHAR *)u##s)

/* the compiler may emit memset for struct zero-initialisation */
void *memset(void *d, int c, unsigned long long n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

#define PORT 47811

static HANDLE out_h, err_h;

static void put(HANDLE h, const char *p, DWORD n)
{
    DWORD w;
    while (n && WriteFile(h, p, n, &w, NULL) && w) { p += w; n -= w; }
}

static int recv_all(SOCKET s, char *p, int n)
{
    while (n > 0) {
        int r = recv(s, p, n, 0);
        if (r <= 0) return 0;
        p += r; n -= r;
    }
    return 1;
}

/* one header line: kind letter + decimal number + '\n' */
static int recv_head(SOCKET s, char *kind, long long *num)
{
    char c; int neg = 0, any = 0;
    *num = 0;
    if (!recv_all(s, kind, 1)) return 0;
    for (;;) {
        if (!recv_all(s, &c, 1)) return 0;
        if (c == '\n') { if (neg) *num = -*num; return any || *kind == 'F'; }
        if (c == '-' && !any && !neg) { neg = 1; continue; }
        if (c < '0' || c > '9' || *num > 0x7fffffff) return 0;
        *num = *num * 10 + (c - '0'); any = 1;
    }
}

/* the command line after argv[0], CommandLineToArgvW's rule for the program name */
static WCHAR *args_tail(WCHAR *p)
{
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
    else while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int fallback(const WCHAR *tail)
{
    static WCHAR exe[MAX_PATH + 32], cmd[32768];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH), code = 1, i = 0;
    const WCHAR *suf = L(".steamac-orig");
    if (!n || n >= MAX_PATH) return 1;
    while (*suf) exe[n++] = *suf++;
    exe[n] = 0;
    cmd[i++] = '"';
    for (const WCHAR *q = exe; *q && i < 32000; ) cmd[i++] = *q++;
    cmd[i++] = '"'; cmd[i++] = ' ';
    while (*tail && i < 32766) cmd[i++] = *tail++;
    cmd[i] = 0;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_h;
    si.hStdError = err_h;
    if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        static const char msg[] = "ERROR: steamac yt-dlp stub: no native yt-dlp and no original yt-dlp.exe\n";
        put(err_h, msg, sizeof(msg) - 1);
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

static int run(void)
{
    static char utf8[3 * 32768], head[64], buf[65536];
    WCHAR *tail = args_tail(GetCommandLineW());
    WSADATA wsa;
    SOCKET s;
    struct sockaddr_in a = { 0 };
    int n, h = 0;
    char kind; long long num;

    out_h = GetStdHandle(STD_OUTPUT_HANDLE);
    err_h = GetStdHandle(STD_ERROR_HANDLE);
    n = WideCharToMultiByte(CP_UTF8, 0, tail, -1, utf8, sizeof(utf8), NULL, NULL);
    if (n <= 0 || WSAStartup(MAKEWORD(2, 2), &wsa)) return fallback(tail);
    n--;    /* without the terminator */
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    a.sin_addr = htonl(INADDR_LOOPBACK);
    if (s == INVALID_SOCKET || connect(s, (struct sockaddr *)&a, sizeof(a))) return fallback(tail);

    /* "STEAMAC-YTDLP 1 <n>\n" */
    {
        const char *p = "STEAMAC-YTDLP 1 ";
        char d[16]; int k = 0, m = n;
        while (*p) head[h++] = *p++;
        do { d[k++] = '0' + m % 10; m /= 10; } while (m);
        while (k) head[h++] = d[--k];
        head[h++] = '\n';
    }
    if (send(s, head, h, 0) != h || (n && send(s, utf8, n, 0) != n)) return fallback(tail);

    for (int got = 0;; got = 1) {
        /* nothing written yet: the original yt-dlp.exe can still answer instead */
        if (!recv_head(s, &kind, &num)) return got ? 1 : fallback(tail);
        if (kind == 'F') { closesocket(s); return fallback(tail); }
        if (kind == 'X') { closesocket(s); return (int)num; }
        if ((kind != 'O' && kind != 'E') || num < 0) return 1;
        while (num > 0) {
            int c = num > (long long)sizeof(buf) ? (int)sizeof(buf) : (int)num;
            if (!recv_all(s, buf, c)) return 1;
            put(kind == 'O' ? out_h : err_h, buf, c);
            num -= c;
        }
    }
}

void __stdcall mainCRTStartup(void)
{
    ExitProcess((UINT)run());
}
