#include "scanbench.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/wait.h>

int sb_execute(const SBConfig *cfg, SBResult *r, SBInventory *inventory) {
    memset(r, 0, sizeof(*r)); memset(inventory, 0, sizeof(*inventory));
    r->completion = SB_COMPLETE; r->verification = SB_UNVERIFIED;
    snprintf(r->verification_basis, sizeof(r->verification_basis), "none");
    r->timestamp_ns = sb_realtime_ns(); sb_mem_limit(cfg->memory_limit);
    uint64_t start = sb_now_ns(); r->total_ns = start;
    int fd = open(cfg->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) sb_failure(r, SB_FAILED, errno, "cannot open root: %s", strerror(errno));
    else { ++r->counters.opens; sb_environment(cfg, r, fd); close(fd); }
#ifndef __APPLE__
    if (cfg->method != SB_POSIX && cfg->method != SB_POSIX_PAR)
        sb_failure(r, SB_UNSUPPORTED, ENOTSUP, "this backend requires macOS; not substituted with POSIX traversal");
    if (cfg->size == SB_ALLOCATED && cfg->task != SB_ENUMERATE)
        sb_failure(r, SB_UNSUPPORTED, ENOTSUP, "canonical ATTR_FILE_ALLOCSIZE accounting requires macOS");
#endif
    if (cfg->skip_empty && cfg->consistency != SB_IMMUTABLE)
        sb_failure(r, SB_UNSUPPORTED, EINVAL, "--skip-empty requires --consistency immutable; assertion is the caller's responsibility");
    if (r->completion == SB_COMPLETE) {
        if (cfg->method >= SB_CATALOG) sb_catalog_run(cfg, r, inventory);
        else sb_walk_run(cfg, r, inventory);
    }
    uint64_t end = sb_now_ns(); r->total_ns = end - start;
    /* Charge all dispatch, teardown and gaps to finalization, preserving a sum. */
    if (r->setup_ns + r->scan_ns <= r->total_ns) r->finalize_ns = r->total_ns - r->setup_ns - r->scan_ns;
    r->managed_peak_bytes = sb_mem_peak();
    struct rusage usage;
    if (!getrusage(RUSAGE_SELF, &usage)) {
        r->user_ns = (uint64_t)usage.ru_utime.tv_sec * UINT64_C(1000000000) + (uint64_t)usage.ru_utime.tv_usec * 1000;
        r->system_ns = (uint64_t)usage.ru_stime.tv_sec * UINT64_C(1000000000) + (uint64_t)usage.ru_stime.tv_usec * 1000;
#ifdef __APPLE__
        r->max_rss_bytes = (uint64_t)usage.ru_maxrss;
#else
        r->max_rss_bytes = (uint64_t)usage.ru_maxrss * 1024;
#endif
    }
    return r->completion == SB_COMPLETE ? 0 : r->completion == SB_UNSUPPORTED ? 2 : 1;
}
static bool timed_header(int fd, void *data, size_t n, uint64_t deadline) {
    unsigned char *p = data;
    while (n) {
        uint64_t now = sb_now_ns();
        if (now >= deadline) { errno = ETIMEDOUT; return false; }
        uint64_t ms = (deadline - now + 999999) / 1000000;
        struct pollfd item = {.fd = fd, .events = POLLIN};
        int rc = poll(&item, 1, ms > INT32_MAX ? INT32_MAX : (int)ms);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) { if (!rc) errno = ETIMEDOUT; return false; }
        ssize_t got = read(fd, p, n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { errno = EPIPE; return false; }
        p += (size_t)got; n -= (size_t)got;
    }
    return true;
}
int sb_spawn(const char *self, const SBConfig *cfg, SBResult *r, SBInventory *inventory) {
    int input[2], output[2]; memset(r, 0, sizeof(*r)); memset(inventory, 0, sizeof(*inventory));
    if (pipe(input)) { sb_failure(r, SB_FAILED, errno, "config pipe failed"); return 1; }
    if (pipe(output)) { close(input[0]); close(input[1]); sb_failure(r, SB_FAILED, errno, "result pipe failed"); return 1; }
    uint64_t start = sb_now_ns();
    pid_t pid = fork();
    if (pid < 0) {
        close(input[0]); close(input[1]); close(output[0]); close(output[1]);
        sb_failure(r, SB_FAILED, errno, "fork failed"); return 1;
    }
    if (!pid) {
        close(input[1]); close(output[0]);
        char in[32], out[32]; snprintf(in, sizeof(in), "%d", input[0]); snprintf(out, sizeof(out), "%d", output[1]);
        char *const argv[] = {(char *)self, "__run", in, out, NULL};
        execv(self, argv); _exit(127);
    }
    close(input[0]); close(output[1]);
    bool ok = sb_write_all(input[1], cfg, sizeof(*cfg)); close(input[1]);
    uint64_t allowance = cfg->timeout_ns ? cfg->timeout_ns : UINT64_C(3600000000000);
    if (allowance < UINT64_MAX - UINT64_C(30000000000)) allowance += UINT64_C(30000000000);
    uint64_t deadline = allowance < UINT64_MAX - start ? start + allowance : UINT64_MAX;
    if (ok) ok = timed_header(output[0], r, sizeof(*r), deadline);
    int read_error = errno;
    if (ok && cfg->keep_manifest) ok = sb_manifest_read(output[0], inventory, cfg->memory_limit);
    if (!ok) kill(pid, SIGKILL);
    close(output[0]); int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    uint64_t elapsed = sb_now_ns() - start;
    if (!ok || !WIFEXITED(status) || WEXITSTATUS(status) > 2) {
        sb_inventory_destroy(inventory); memset(r, 0, sizeof(*r));
        int err = !ok ? read_error : ECHILD;
        sb_failure(r, SB_FAILED, err, "worker process failed (exit=%d, signal=%d); possible resource limit, timeout, or crash",
            WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        r->timestamp_ns = sb_realtime_ns();
    }
    r->process_wall_ns = elapsed;
    return r->completion == SB_COMPLETE ? 0 : r->completion == SB_UNSUPPORTED ? 2 : 1;
}
