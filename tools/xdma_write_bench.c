#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv) {
    const char *dev = "/dev/xdma0_h2c_0";
    size_t buf_size = 2 * 1024 * 1024; // 2MB
    int iterations = 100;
    if (argc > 1) dev = argv[1];
    if (argc > 2) buf_size = strtoul(argv[2], NULL, 10);
    if (argc > 3) iterations = atoi(argv[3]);

    int fd = open(dev, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", dev, strerror(errno));
        return 1;
    }

    void *buf = NULL;
    if (posix_memalign(&buf, 4096, buf_size) != 0) {
        fprintf(stderr, "posix_memalign failed\n");
        close(fd);
        return 1;
    }
    memset(buf, 0xA5, buf_size);

    struct timespec t0, t1;
    if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0) {
        perror("clock_gettime");
        free(buf);
        close(fd);
        return 1;
    }

    size_t total = 0;
    for (int i = 0; i < iterations; ++i) {
        ssize_t written = write(fd, buf, buf_size);
        if (written < 0) {
            fprintf(stderr, "write failed at iter %d: %s\n", i, strerror(errno));
            break;
        }
        total += (size_t)written;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &t1) != 0) {
        perror("clock_gettime");
        free(buf);
        close(fd);
        return 1;
    }

    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("wrote %zu bytes in %.3f s -> %.3f MB/s\n", total, elapsed, (total / (1024.0*1024.0)) / elapsed);

    free(buf);
    close(fd);
    return 0;
}
