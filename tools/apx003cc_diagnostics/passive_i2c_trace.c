/*
 * passive_i2c_trace.c — LD_PRELOAD interposer that records every userspace I2C
 * transaction on /dev/i2c-* (and flags /dev/video* ioctls in case the vendor
 * lib drives the sensor through a v4l2 subdev instead of raw i2c-dev).
 *
 * Rationale (2026-10-04): the vendor sensor lib writes registers through the
 * x5-libcam symbols write_array / camera_reg_i2c_write8 / read_reg16_data8,
 * but the board run proved camera_reg_i2c_write8 is NOT the init path (it was
 * never called while the sensor was fully initialised). Intercepting the
 * lowest common denominator — open()+ioctl(I2C_RDWR) on /dev/i2c-* — captures
 * write8 AND write_array alike, because both ultimately issue I2C_RDWR to the
 * i2c-dev node (sensor 0x3c has no kernel driver bound; PRIOR_KNOWLEDGE 3.2#17).
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
 * Output lines (parsed by parse_i2c_trace.py):
 *   OPEN <path> fd=<n>
 *   I2C_SLAVE fd=<n> addr=0x<aa>
 *   I2C_WR fd=<n> addr=0x<aa> reg=0x<RRRR> data=<hex bytes>
 *   I2C_RD fd=<n> addr=0x<aa> reg=0x<RRRR> len=<n> -> <hex bytes>
 *   I2C_MSG fd=<n> addr=0x<aa> flags=0x<ffff> len=<n> data=<hex bytes>
 *   IOCTL fd=<n> req=0x<hex> ret=<n>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>

/* --- minimal self-contained I2C ABI (linux/i2c.h + linux/i2c-dev.h) --- */
#define I2C_SLAVE       0x0703
#define I2C_SLAVE_FORCE 0x0706
#define I2C_RDWR        0x0707
#define I2C_SMBUS       0x0720
#define I2C_M_RD        0x0001
#define I2C_M_TEN       0x0010

struct i2c_msg {
    uint16_t addr;
    uint16_t flags;
    uint16_t len;
    uint8_t *buf;
};
struct i2c_rdwr_ioctl_data {
    struct i2c_msg *msgs;
    uint32_t nmsgs;
};

/* --- real libc symbols --- */
static int  (*real_open)(const char *, int, ...);
static int  (*real_open64)(const char *, int, ...);
static int  (*real_openat)(int, const char *, int, ...);
static int  (*real_openat64)(int, const char *, int, ...);
static int  (*real_close)(int);
static int  (*real_ioctl)(int, unsigned long, ...);
static ssize_t (*real_read)(int, void *, size_t);
static ssize_t (*real_write)(int, const void *, size_t);

/* --- fd -> path map --- */
#define MAX_FDS 1024
static char g_fd_path[MAX_FDS][256];

/* --- logging --- */
static int g_fd = -1;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static __thread int g_in_log = 0;

__attribute__((constructor)) static void trace_banner(void) {
    const char *p = getenv("I2C_TRACE_LOG");
    fprintf(stderr, "APX_TRACE_LOADED pid=%d log=%s\n",
            (int)getpid(), (p && p[0]) ? p : "(unset -> stderr)");
}

static void open_log(void) {
    if (g_fd >= 0) return;
    const char *p = getenv("I2C_TRACE_LOG");
    if (p && p[0]) {
        if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
        g_fd = real_open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (g_fd >= 0) {
            dprintf(g_fd, "# APX_TRACE i2c interposer opened pid=%d\n", (int)getpid());
        }
    }
    if (g_fd < 0) g_fd = 2; /* stderr fallback */
}

static void trace_log(const char *fmt, ...) {
    if (g_in_log) return;
    g_in_log = 1;
    pthread_mutex_lock(&g_mutex);
    open_log();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    dprintf(g_fd, "[%ld.%06ld] ", (long)ts.tv_sec, (long)(ts.tv_nsec / 1000));
    va_list ap;
    va_start(ap, fmt);
    vdprintf(g_fd, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&g_mutex);
    g_in_log = 0;
}

static const char *fd_path(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return "";
    return g_fd_path[fd][0] ? g_fd_path[fd] : "";
}
static int is_i2c(int fd) { return strstr(fd_path(fd), "/dev/i2c") != NULL; }
static int is_video(int fd) { return strstr(fd_path(fd), "/dev/video") != NULL; }

static void record_fd(int fd, const char *path) {
    if (fd < 0 || fd >= MAX_FDS) return;
    if (path) { strncpy(g_fd_path[fd], path, 255); g_fd_path[fd][255] = 0; }
    else g_fd_path[fd][0] = 0;
    if (path && (strstr(path, "/dev/i2c") || strstr(path, "/dev/video")))
        trace_log("OPEN %s fd=%d", path, fd);
}

static void hexdump(const uint8_t *b, uint16_t n, char *out, size_t cap) {
    size_t o = 0;
    for (uint16_t i = 0; i < n && o + 4 < cap; i++)
        o += (size_t)snprintf(out + o, cap - o, "%02x ", b[i]);
    if (o && out[o - 1] == ' ') out[o - 1] = 0;
}

/* --- open family --- */
int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
    int fd = real_open(path, flags, mode);
    record_fd(fd, path);
    return fd;
}
int open64(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    if (!real_open64) real_open64 = dlsym(RTLD_NEXT, "open64");
    int fd = real_open64(path, flags, mode);
    record_fd(fd, path);
    return fd;
}
int openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    if (!real_openat) real_openat = dlsym(RTLD_NEXT, "openat");
    int fd = real_openat(dirfd, path, flags, mode);
    record_fd(fd, path);
    return fd;
}
int openat64(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    if (!real_openat64) real_openat64 = dlsym(RTLD_NEXT, "openat64");
    int fd = real_openat64(dirfd, path, flags, mode);
    record_fd(fd, path);
    return fd;
}

int close(int fd) {
    if (!real_close) real_close = dlsym(RTLD_NEXT, "close");
    if (fd >= 0 && fd < MAX_FDS) g_fd_path[fd][0] = 0;
    return real_close(fd);
}

/* --- ioctl: parse I2C_SLAVE / I2C_RDWR; tag /dev/video ioctls --- */
int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    int ret = real_ioctl(fd, request, arg);

    if (is_i2c(fd)) {
        switch (request) {
        case I2C_SLAVE:
            trace_log("I2C_SLAVE fd=%d addr=0x%02lx ret=%d", fd, (unsigned long)arg & 0x7fUL, ret);
            break;
        case I2C_SLAVE_FORCE:
            trace_log("I2C_SLAVE_FORCE fd=%d addr=0x%02lx ret=%d", fd, (unsigned long)arg & 0x7fUL, ret);
            break;
        case I2C_RDWR: {
            struct i2c_rdwr_ioctl_data *rd = (struct i2c_rdwr_ioctl_data *)arg;
            for (uint32_t i = 0; i < rd->nmsgs; i++) {
                struct i2c_msg *m = &rd->msgs[i];
                char hex[512];
                hexdump(m->buf, m->len, hex, sizeof hex);
                if ((m->flags & I2C_M_RD) == 0 && m->len >= 2) {
                    uint16_t reg = (uint16_t)((m->buf[0] << 8) | m->buf[1]);
                    if (m->len >= 3) {
                        /* reg16 + data8 (single or burst): data follows the addr */
                        trace_log("I2C_WR fd=%d addr=0x%02x reg=0x%04x data=%s",
                                  fd, m->addr, reg, hex + 6);
                    } else {
                        /* bare 2-byte register address (likely part of a read) */
                        trace_log("I2C_ADDR fd=%d addr=0x%02x reg=0x%04x", fd, m->addr, reg);
                    }
                } else if (m->flags & I2C_M_RD) {
                    trace_log("I2C_RD fd=%d addr=0x%02x len=%u -> %s ret=%d",
                              fd, m->addr, m->len, hex, ret);
                } else {
                    trace_log("I2C_MSG fd=%d addr=0x%02x flags=0x%04x len=%u data=%s",
                              fd, m->addr, m->flags, m->len, hex);
                }
            }
            break;
        }
        case I2C_SMBUS:
            trace_log("I2C_SMBUS fd=%d ret=%d", fd, ret);
            break;
        default:
            trace_log("IOCTL fd=%d req=0x%lx ret=%d", fd, request, ret);
            break;
        }
    } else if (is_video(fd)) {
        trace_log("IOCTL_VIDEO fd=%d req=0x%lx ret=%d", fd, request, ret);
    }
    return ret;
}

/* --- read/write: raw i2c-dev fallback path (only log i2c fds) --- */
ssize_t read(int fd, void *buf, size_t count) {
    if (!real_read) real_read = dlsym(RTLD_NEXT, "read");
    ssize_t ret = real_read(fd, buf, count);
    if (is_i2c(fd) && ret > 0) {
        char hex[512];
        hexdump((const uint8_t *)buf, (uint16_t)(ret < 512 ? ret : 512), hex, sizeof hex);
        trace_log("READ fd=%d len=%zd -> %s", fd, ret, hex);
    }
    return ret;
}
ssize_t write(int fd, const void *buf, size_t count) {
    if (!real_write) real_write = dlsym(RTLD_NEXT, "write");
    ssize_t ret = real_write(fd, buf, count);
    if (is_i2c(fd) && ret > 0) {
        char hex[512];
        hexdump((const uint8_t *)buf, (uint16_t)(count < 256 ? count : 256), hex, sizeof hex);
        trace_log("WRITE fd=%d len=%zd data=%s", fd, ret, hex);
    }
    return ret;
}
