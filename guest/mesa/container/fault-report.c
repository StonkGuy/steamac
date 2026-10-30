// SPDX-License-Identifier: MIT
// steamac fault reporter for emulated x86_64 games (FEX), loaded only through the game's FEX
// AppConfig (Steam launch options cannot: FEX's compat tool removes LD_PRELOAD):
// compatdata/<appid>/fex-emu/AppConfig/<executable>.json =
// {"Config":{"Env":"LD_PRELOAD=/usr/lib/steamac/x86_64/fault-report.so"}}
//
// FEX re-raises an unhandled guest SIGSEGV from its JIT code, so the core dump of an emulated
// game shows an anonymous AArch64 address. This library runs inside the guest process: on
// SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT it appends the guest x86_64 RIP, the fault address, the
// registers, a backtrace and the module+offset of every frame (from /proc/self/maps) to
// $HOME/.local/state/steamac/fault-report.txt (STEAMAC_FAULT_REPORT overrides the path), then
// hands the signal to the game's own handler or the default action, so the crash and its core
// dump happen as before. The file is kept under 256 KiB (truncated before a report that would
// exceed it) and each process writes at most 4 reports.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#define FILE_LIMIT (256 << 10)
#define MAPS_LIMIT (64 << 10)
#define MAX_REPORTS 4

static const int fault_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
static struct sigaction app_action[NSIG];
static int (*real_sigaction)(int, const struct sigaction *, struct sigaction *);
static char report_path[512], report_dir[512];
static char maps[MAPS_LIMIT + 1];
static size_t maps_len;
static volatile sig_atomic_t reporting, reports;

static int is_fault_signal(int sig) {
    for (size_t i = 0; i < sizeof fault_signals / sizeof *fault_signals; i++)
        if (fault_signals[i] == sig) return 1;
    return 0;
}

static void put(int fd, const char *s, size_t n) {
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

__attribute__((format(printf, 2, 3))) static void putf(int fd, const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) put(fd, b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
}

static void load_maps(void) {
    maps_len = 0;
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t n;
    while (maps_len < MAPS_LIMIT && (n = read(fd, maps + maps_len, MAPS_LIMIT - maps_len)) > 0) maps_len += (size_t)n;
    maps[maps_len] = 0;
    close(fd);
}

// Parses a hex number at *p, advances *p past it.
static uintptr_t hex(const char **p, const char *end) {
    uintptr_t v = 0;
    for (; *p < end; ++*p) {
        char c = **p;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) break;
        v = v << 4 | (uintptr_t)d;
    }
    return v;
}

static const char *skip_field(const char *p, const char *end) {
    while (p < end && *p != ' ') p++;
    while (p < end && *p == ' ') p++;
    return p;
}

// Mapping that contains addr ("lo-hi perm offset dev inode path" lines): returns 1 and fills
// the fields, 0 if unmapped (or past MAPS_LIMIT). No sscanf: this runs in a signal handler.
static int find_mapping(uintptr_t addr, uintptr_t *start, uintptr_t *stop, uintptr_t *offset, char *perm, char *path,
                        size_t path_size) {
    for (const char *p = maps; p < maps + maps_len;) {
        const char *end = memchr(p, '\n', maps + maps_len - p);
        if (!end) break;
        const char *q = p;
        uintptr_t lo = hex(&q, end);
        q++;
        uintptr_t hi = hex(&q, end);
        if (addr >= lo && addr < hi && end - q > 6) {
            memcpy(perm, q + 1, 4);
            perm[4] = 0;
            q = skip_field(q + 1, end);
            *offset = hex(&q, end);
            q = skip_field(skip_field(skip_field(q, end), end), end); // offset, dev, inode
            size_t len = (size_t)(end - q) < path_size - 1 ? (size_t)(end - q) : path_size - 1;
            memcpy(path, q, len);
            path[len] = 0;
            *start = lo;
            *stop = hi;
            return 1;
        }
        p = end + 1;
    }
    return 0;
}

static void describe(int fd, uintptr_t addr) {
    uintptr_t start, stop, offset;
    char perm[5], path[256];
    if (!find_mapping(addr, &start, &stop, &offset, perm, path, sizeof path)) {
        put(fd, " [unmapped]", 11);
        return;
    }
    putf(fd, " [%s %s +0x%lx]", path[0] ? path : "anon", perm, (unsigned long)(addr - start + offset));
    Dl_info info;
    if (dladdr((void *)addr, &info) && info.dli_sname)
        putf(fd, " %s+0x%lx", info.dli_sname, (unsigned long)(addr - (uintptr_t)info.dli_saddr));
}

static int open_report(void) {
    // mkdir -p of the parent: only the last two components can be missing (~/.local/state/steamac).
    char parent[512];
    snprintf(parent, sizeof parent, "%s", report_dir);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = 0;
        mkdir(parent, 0755);
    }
    mkdir(report_dir, 0755);
    struct stat st;
    int flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
    if (stat(report_path, &st) == 0 && st.st_size > FILE_LIMIT - MAPS_LIMIT - (16 << 10)) flags |= O_TRUNC;
    return open(report_path, flags, 0644);
}

static void report(int fd, int sig, const siginfo_t *si, const ucontext_t *uc) {
    const greg_t *r = uc->uc_mcontext.gregs;
    char comm[32] = "";
    int cfd = open("/proc/self/comm", O_RDONLY | O_CLOEXEC);
    if (cfd >= 0) {
        ssize_t n = read(cfd, comm, sizeof comm - 1);
        comm[n > 0 ? n - 1 : 0] = 0;
        close(cfd);
    }
    putf(fd, "=== fault-report pid %d tid %ld comm %s signal %d code %d addr %p\n", getpid(),
         (long)syscall(SYS_gettid), comm, sig, si->si_code, si->si_addr);
    putf(fd, "rip %016llx", (unsigned long long)r[REG_RIP]);
    describe(fd, (uintptr_t)r[REG_RIP]);
    putf(fd, "\nrax %016llx rbx %016llx rcx %016llx rdx %016llx\n", (unsigned long long)r[REG_RAX],
         (unsigned long long)r[REG_RBX], (unsigned long long)r[REG_RCX], (unsigned long long)r[REG_RDX]);
    putf(fd, "rsi %016llx rdi %016llx rbp %016llx rsp %016llx\n", (unsigned long long)r[REG_RSI],
         (unsigned long long)r[REG_RDI], (unsigned long long)r[REG_RBP], (unsigned long long)r[REG_RSP]);
    putf(fd, "r8  %016llx r9  %016llx r10 %016llx r11 %016llx\n", (unsigned long long)r[REG_R8],
         (unsigned long long)r[REG_R9], (unsigned long long)r[REG_R10], (unsigned long long)r[REG_R11]);
    putf(fd, "r12 %016llx r13 %016llx r14 %016llx r15 %016llx\n", (unsigned long long)r[REG_R12],
         (unsigned long long)r[REG_R13], (unsigned long long)r[REG_R14], (unsigned long long)r[REG_R15]);

    void *frames[48];
    int n = backtrace(frames, 48);
    put(fd, "backtrace:\n", 11);
    for (int i = 0; i < n; i++) {
        putf(fd, "#%-2d %p", i, frames[i]);
        describe(fd, (uintptr_t)frames[i]);
        put(fd, "\n", 1);
    }
    // Return-address candidates on the faulting stack: unwinding stops at frames without CFI.
    put(fd, "stack scan:\n", 12);
    const uintptr_t *sp = (const uintptr_t *)r[REG_RSP];
    uintptr_t start, stack_end, offset;
    char perm[5], path[256];
    if (find_mapping((uintptr_t)sp, &start, &stack_end, &offset, perm, path, sizeof path)) {
        uintptr_t map_start, map_end;
        for (int i = 0, shown = 0; shown < 32 && i < 4096 && (uintptr_t)(sp + i + 1) <= stack_end; i++) {
            if (!find_mapping(sp[i], &map_start, &map_end, &offset, perm, path, sizeof path) || perm[2] != 'x' ||
                path[0] != '/')
                continue;
            putf(fd, "  rsp+0x%x %lx", i * 8, (unsigned long)sp[i]);
            describe(fd, sp[i]);
            put(fd, "\n", 1);
            shown++;
        }
    }
    put(fd, "maps:\n", 6);
    put(fd, maps, maps_len);
    put(fd, "=== end\n", 8);
}

static void on_fault(int sig, siginfo_t *si, void *ucontext) {
    if (!reporting && reports < MAX_REPORTS) {
        reporting = 1;
        reports++;
        load_maps();
        int fd = open_report();
        if (fd >= 0) {
            report(fd, sig, si, ucontext);
            close(fd);
        }
        putf(2, "steamac fault-report: signal %d, details in %s\n", sig, report_path);
        reporting = 0;
    }
    const struct sigaction *app = &app_action[sig];
    if ((app->sa_flags & SA_SIGINFO) && app->sa_sigaction) {
        app->sa_sigaction(sig, si, ucontext);
        return;
    }
    if (!(app->sa_flags & SA_SIGINFO) && app->sa_handler != SIG_DFL && app->sa_handler != SIG_IGN) {
        app->sa_handler(sig);
        return;
    }
    struct sigaction dfl = {0};
    dfl.sa_handler = SIG_DFL;
    real_sigaction(sig, &dfl, NULL);
    // A hardware fault repeats on return and now takes the default action (core dump);
    // a signal sent with kill/raise/abort must be sent again.
    if (si->si_code <= 0) raise(sig);
}

// The game's own handlers for the fault signals are recorded and chained, not installed.
int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    if (!real_sigaction) real_sigaction = dlsym(RTLD_NEXT, "sigaction");
    if (sig > 0 && sig < NSIG && is_fault_signal(sig)) {
        if (old) *old = app_action[sig];
        if (act) app_action[sig] = *act;
        return 0;
    }
    return real_sigaction(sig, act, old);
}

sighandler_t signal(int sig, sighandler_t handler) {
    struct sigaction act = {0}, old;
    act.sa_handler = handler;
    act.sa_flags = SA_RESTART;
    if (sigaction(sig, &act, &old) < 0) return SIG_ERR;
    return old.sa_handler;
}

__attribute__((constructor)) static void fault_report_init(void) {
    real_sigaction = dlsym(RTLD_NEXT, "sigaction");
    if (!real_sigaction) return;
    const char *path = getenv("STEAMAC_FAULT_REPORT"), *home = getenv("HOME");
    if (path && *path)
        snprintf(report_path, sizeof report_path, "%s", path);
    else
        snprintf(report_path, sizeof report_path, "%s/.local/state/steamac/fault-report.txt", home && *home ? home : "/tmp");
    snprintf(report_dir, sizeof report_dir, "%s", report_path);
    char *slash = strrchr(report_dir, '/');
    if (slash) *slash = 0;
    void *warm[2];
    backtrace(warm, 2); // loads libgcc_s now rather than inside the signal handler
    // Faults from stack overflows need their own stack. Threads created later share this
    // process-wide handler but not the alternate stack (sigaltstack is per thread).
    static char altstack[128 << 10];
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack};
    sigaltstack(&ss, NULL);
    for (size_t i = 0; i < sizeof fault_signals / sizeof *fault_signals; i++) {
        struct sigaction act = {0};
        act.sa_sigaction = on_fault;
        act.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
        real_sigaction(fault_signals[i], &act, NULL);
    }
}
