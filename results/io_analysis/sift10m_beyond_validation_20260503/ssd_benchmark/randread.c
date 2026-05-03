#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <errno.h>

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <filename> <block_size_kb> <num_reads> [num_threads]\n", argv[0]);
        return 1;
    }

    char *filename = argv[1];
    int block_size = atoi(argv[2]) * 1024;  // KB to bytes
    int num_reads = atoi(argv[3]);
    int num_threads = argc > 4 ? atoi(argv[4]) : 1;

    int fd = open(filename, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        perror("open (O_DIRECT failed, trying without)");
        fd = open(filename, O_RDONLY);
        if (fd < 0) {
            perror("open");
            return 1;
        }
    }

    // Get file size
    off_t file_size = lseek(fd, 0, SEEK_END);
    off_t num_blocks = file_size / block_size;

    // Allocate aligned buffer
    void *buf;
    if (posix_memalign(&buf, 4096, block_size) != 0) {
        perror("posix_memalign");
        close(fd);
        return 1;
    }

    // Random reads
    srand(time(NULL) ^ getpid());
    for (int i = 0; i < num_reads; i++) {
        off_t offset = (rand() % num_blocks) * block_size;
        lseek(fd, offset, SEEK_SET);
        ssize_t n = read(fd, buf, block_size);
        if (n != block_size) {
            fprintf(stderr, "read error at offset %ld: %d (%s)\n", offset, errno, strerror(errno));
        }
    }

    free(buf);
    close(fd);
    return 0;
}
