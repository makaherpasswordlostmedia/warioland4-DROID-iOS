#include "WL4Log.h"
#include "host.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void on_fatal_signal(int sig, siginfo_t *si, void *ctx) {
    (void)ctx;
    char m[96];
    int n = snprintf(m, sizeof m, "FATAL native signal %d, fault addr %p\n", sig, si ? si->si_addr : 0);
    if (n > 0) { (void)!write(STDERR_FILENO, m, (size_t)n); }
    wl4_ring_write(STDERR_FILENO);
    signal(sig, SIG_DFL);
    raise(sig);
}

void wl4_log_open(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (fd >= 0) {
        dup2(fd, STDERR_FILENO);
        dup2(fd, STDOUT_FILENO);
        if (fd > STDERR_FILENO) close(fd);
        setvbuf(stdout, NULL, _IOLBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
    }
    char d[1024];
    snprintf(d, sizeof d, "%s.stall.bin", path);
    wl4_set_dump_path(d);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fatal_signal;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    fprintf(stderr, "wl4: log opened (%s)\n", path);
}
