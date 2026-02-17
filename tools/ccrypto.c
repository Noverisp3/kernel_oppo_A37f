#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <endian.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define CINNAMON_SEED 0x9E3779B97F4A7C15ULL
#define CINNAMON_ROTATE_BITS 13
#define CINNAMON_ZERO_CONSTANT 0x5A5A5A5A5A5A5A5AULL

static uint64_t cinnamon_checksum(const uint8_t *data, size_t len)
{
    uint64_t checksum = CINNAMON_SEED;
    size_t i;

    for (i = 0; i + 8 <= len; i += 8) {
        uint64_t chunk;
        memcpy(&chunk, data + i, 8);
        chunk = le64toh(chunk);

        if (chunk == 0) {
            checksum ^= CINNAMON_ZERO_CONSTANT;
        } else {
            checksum ^= chunk;
            checksum = (checksum << CINNAMON_ROTATE_BITS) |
                       (checksum >> (64 - CINNAMON_ROTATE_BITS));
        }
    }

    if (len % 8) {
        uint64_t tail = 0;
        memcpy(&tail, data + i, len % 8);
        checksum ^= tail;
    }

    return checksum;
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <file_path or string>\n", argv[0]);
        return 1;
    }

    const char *input = argv[1];
    uint8_t *data = NULL;
    size_t len = 0;

    if (input[0] == '/') {  // Assume file path
        int fd = open(input, O_RDONLY);
        if (fd < 0) {
            perror("open");
            return 1;
        }
        struct stat st;
        if (fstat(fd, &st) < 0) {
            perror("fstat");
            close(fd);
            return 1;
        }
        len = st.st_size;
        data = malloc(len);
        if (!data) {
            perror("malloc");
            close(fd);
            return 1;
        }
        if (read(fd, data, len) != len) {
            perror("read");
            free(data);
            close(fd);
            return 1;
        }
        close(fd);
    } else {  // String
        len = strlen(input);
        data = (uint8_t *)input;
    }

    uint64_t checksum = cinnamon_checksum(data, len);
    printf("0x%016llx\n", (unsigned long long)checksum);

    if (input[0] == '/') {
        free(data);
    }

    return 0;
}
