#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>

/*
 * passive_i2c_trace.c — LD_PRELOAD interposer over the APX003CC vendor sensor
 * lib's register-write entry point. It forwards every call unchanged (never
 * adds, drops, or rewrites a register write) and logs the full (bus, width,
 * addr, reg, value) tuple so we can ground-truth the real register write
 * sequence the vendor stack issues during stream open/init.
 *
 * Signature source: D-Robotics/x5-libcam-inc develop/camera_reg.h
 *   int32_t camera_reg_i2c_write8(int32_t bus, int32_t width, int32_t addr,
 *                                 uint32_t reg, uint8_t value);
 *
 * Build (RDK X5 board, aarch64 gcc):
 *   gcc -shared -fPIC -O2 -o passive_i2c_trace.so passive_i2c_trace.c -ldl -lpthread
 *
 * Run:
 *   export I2C_TRACE_LOG=/tmp/i2c_trace.log
 *   LD_PRELOAD=$PWD/tools/apx003cc_diagnostics/passive_i2c_trace.so \
 *     python3 hvs.py live --x5-vin-bypass -- --preview-width 960 --no-verify \
 *     2>&1 | tee /tmp/preview_terminal.log
 *   # ~15s, then Ctrl+C
 *
 * Diagnostics (decides whether write8 or a block-write symbol is the real path):
 *   - "APX_TRACE_LOADED" on stderr  => LD_PRELOAD took effect, .so was loaded.
 *   - "APX_TRACE_SYMS write_array=present/absent ..." => which vendor symbols
 *     exist in the next scope (printed on first write8 call).
 *   - "I2C_WR ..." lines => the vendor stack really does drive registers via
 *     camera_reg_i2c_write8; parse with parse_i2c_trace.py.
 */
static int32_t (*real_write)(int32_t, int32_t, int32_t, uint32_t, uint8_t);
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
static int log_fd = -1;

/* constructor: fires the moment the .so is loaded, proving LD_PRELOAD worked. */
__attribute__((constructor)) static void trace_banner(void) {
    const char *p = getenv("I2C_TRACE_LOG");
    fprintf(stderr, "APX_TRACE_LOADED pid=%d log=%s\n",
            (int)getpid(), (p && p[0]) ? p : "(unset -> stderr)");
}

static void resolve(void) {
    *(void **)(&real_write) = dlsym(RTLD_NEXT, "camera_reg_i2c_write8");
    if (!real_write) {
        fprintf(stderr, "APX_TRACE: camera_reg_i2c_write8 NOT resolvable via RTLD_NEXT\n");
        abort();
    }
    fprintf(stderr,
            "APX_TRACE_SYMS write_array=%s read_reg16_data8=%s gpio_power_ctrl=%s\n",
            dlsym(RTLD_NEXT, "write_array") ? "present" : "absent",
            dlsym(RTLD_NEXT, "read_reg16_data8") ? "present" : "absent",
            dlsym(RTLD_NEXT, "gpio_power_ctrl") ? "present" : "absent");
}

static void open_log(void) {
    const char *p = getenv("I2C_TRACE_LOG");
    if (p && p[0]) {
        log_fd = open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (log_fd >= 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            dprintf(log_fd, "# APX_TRACE pid=%d opened\n", (int)getpid());
        }
    }
    if (log_fd < 0) log_fd = 2; /* fall back to stderr */
}

static void log_line(const char *fmt, ...) {
    pthread_mutex_lock(&log_mu);
    if (log_fd < 0) open_log();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    dprintf(log_fd, "I2C_WR t=%ld.%09ld ", (long)ts.tv_sec, (long)ts.tv_nsec);
    va_list ap;
    va_start(ap, fmt);
    vdprintf(log_fd, fmt, ap);
    va_end(ap);
    dprintf(log_fd, "\n");
    pthread_mutex_unlock(&log_mu);
}

int32_t camera_reg_i2c_write8(int32_t bus, int32_t width, int32_t addr, uint32_t reg, uint8_t value) {
    pthread_once(&once, resolve);
    int32_t ret = real_write(bus, width, addr, reg, value);
    log_line("bus=%d width=%d addr=0x%x reg=0x%x value=0x%x ret=%d",
             bus, width, addr, reg, value, ret);
    return ret;
}
