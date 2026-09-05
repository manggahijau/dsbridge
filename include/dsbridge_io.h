#ifndef DSBRIDGE_IO_H
#define DSBRIDGE_IO_H

#include <unistd.h>
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Full write, retrying on EINTR/short write. Returns 0 on success, -1 on error. */
static inline int dsbridge_write_all(int fd, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

/* Full read, retrying on EINTR/short read. Returns 0 on success, -1 on error/EOF. */
static inline int dsbridge_read_all(int fd, void *buf, size_t len) {
    unsigned char *p = (unsigned char *)buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = read(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; /* peer closed */
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* DSBRIDGE_IO_H */
