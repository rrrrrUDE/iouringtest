#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter 426
#endif

#define RING_ENTRIES 8
#define TEST_PATH "/data/local/tmp/iouringtest-data.txt"
#define INITIAL_DATA "io_uring read test\n"
#define REPLACEMENT_DATA "io_uring write PASS\n"

struct ring {
    int fd;
    struct io_uring_params p;
    void *sq_ring_ptr;
    size_t sq_ring_sz;
    void *cq_ring_ptr;
    size_t cq_ring_sz;
    struct io_uring_sqe *sqes;
    size_t sqes_sz;
    unsigned *sq_head, *sq_tail, *sq_mask, *sq_entries, *sq_array;
    unsigned *cq_head, *cq_tail, *cq_mask, *cq_entries;
    struct io_uring_cqe *cqes;
};

static int ring_init(struct ring *r)
{
    memset(r, 0, sizeof(*r));
    r->fd = -1;
    r->p = (struct io_uring_params){0};

    r->fd = (int)syscall(__NR_io_uring_setup, RING_ENTRIES, &r->p);
    if (r->fd < 0) {
        fprintf(stderr, "FAIL: io_uring_setup: errno=%d (%s)\n",
                errno, strerror(errno));
        return -1;
    }

    r->sq_ring_sz = r->p.sq_off.array +
                    r->p.sq_entries * sizeof(unsigned);
    r->cq_ring_sz = r->p.cq_off.cqes +
                    r->p.cq_entries * sizeof(struct io_uring_cqe);

    if (r->p.features & IORING_FEAT_SINGLE_MMAP) {
        if (r->cq_ring_sz > r->sq_ring_sz)
            r->sq_ring_sz = r->cq_ring_sz;
        r->cq_ring_sz = r->sq_ring_sz;
    }

    r->sq_ring_ptr = mmap(NULL, r->sq_ring_sz, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_POPULATE, r->fd,
                          IORING_OFF_SQ_RING);
    if (r->sq_ring_ptr == MAP_FAILED) {
        r->sq_ring_ptr = NULL;
        perror("FAIL: mmap SQ ring");
        return -1;
    }

    if (r->p.features & IORING_FEAT_SINGLE_MMAP) {
        r->cq_ring_ptr = r->sq_ring_ptr;
    } else {
        r->cq_ring_ptr = mmap(NULL, r->cq_ring_sz,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_POPULATE, r->fd,
                              IORING_OFF_CQ_RING);
        if (r->cq_ring_ptr == MAP_FAILED) {
            r->cq_ring_ptr = NULL;
            perror("FAIL: mmap CQ ring");
            return -1;
        }
    }

    r->sqes_sz = r->p.sq_entries * sizeof(struct io_uring_sqe);
    r->sqes = mmap(NULL, r->sqes_sz, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQES);
    if (r->sqes == MAP_FAILED) {
        r->sqes = NULL;
        perror("FAIL: mmap SQEs");
        return -1;
    }

    r->sq_head = (unsigned *)((char *)r->sq_ring_ptr + r->p.sq_off.head);
    r->sq_tail = (unsigned *)((char *)r->sq_ring_ptr + r->p.sq_off.tail);
    r->sq_mask = (unsigned *)((char *)r->sq_ring_ptr + r->p.sq_off.ring_mask);
    r->sq_entries = (unsigned *)((char *)r->sq_ring_ptr + r->p.sq_off.ring_entries);
    r->sq_array = (unsigned *)((char *)r->sq_ring_ptr + r->p.sq_off.array);
    r->cq_head = (unsigned *)((char *)r->cq_ring_ptr + r->p.cq_off.head);
    r->cq_tail = (unsigned *)((char *)r->cq_ring_ptr + r->p.cq_off.tail);
    r->cq_mask = (unsigned *)((char *)r->cq_ring_ptr + r->p.cq_off.ring_mask);
    r->cq_entries = (unsigned *)((char *)r->cq_ring_ptr + r->p.cq_off.ring_entries);
    r->cqes = (struct io_uring_cqe *)((char *)r->cq_ring_ptr + r->p.cq_off.cqes);

    printf("PASS: io_uring_setup; SQ=%u CQ=%u features=0x%x\n",
           r->p.sq_entries, r->p.cq_entries, r->p.features);
    printf("PASS: mmap SQ ring, CQ ring and SQEs\n");
    return 0;
}

static void ring_cleanup(struct ring *r)
{
    if (r->sqes)
        munmap(r->sqes, r->sqes_sz);
    if (r->cq_ring_ptr && r->cq_ring_ptr != r->sq_ring_ptr)
        munmap(r->cq_ring_ptr, r->cq_ring_sz);
    if (r->sq_ring_ptr)
        munmap(r->sq_ring_ptr, r->sq_ring_sz);
    if (r->fd >= 0)
        close(r->fd);
}

static int submit_io(struct ring *r, int fd, unsigned char opcode,
                     void *buf, unsigned len, uint64_t offset,
                     uint64_t user_data)
{
    unsigned head = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
    unsigned tail = __atomic_load_n(r->sq_tail, __ATOMIC_RELAXED);

    if (tail - head >= *r->sq_entries) {
        fprintf(stderr, "FAIL: SQ is full\n");
        return -1;
    }

    unsigned index = tail & *r->sq_mask;
    struct io_uring_sqe *sqe = &r->sqes[index];
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = opcode;
    sqe->fd = fd;
    sqe->off = offset;
    sqe->addr = (uint64_t)(uintptr_t)buf;
    sqe->len = len;
    sqe->user_data = user_data;
    r->sq_array[index] = index;
    __atomic_store_n(r->sq_tail, tail + 1, __ATOMIC_RELEASE);

    long ret = syscall(__NR_io_uring_enter, r->fd, 1, 1,
                       IORING_ENTER_GETEVENTS, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "FAIL: io_uring_enter: errno=%d (%s)\n",
                errno, strerror(errno));
        return -1;
    }

    for (unsigned tries = 0; tries < 1000000; ++tries) {
        unsigned c_head = __atomic_load_n(r->cq_head, __ATOMIC_RELAXED);
        unsigned c_tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);
        if (c_head != c_tail) {
            struct io_uring_cqe cqe = r->cqes[c_head & *r->cq_mask];
            __atomic_store_n(r->cq_head, c_head + 1, __ATOMIC_RELEASE);
            if (cqe.user_data != user_data) {
                fprintf(stderr, "FAIL: unexpected CQE user_data=%llu\n",
                        (unsigned long long)cqe.user_data);
                return -1;
            }
            if (cqe.res < 0) {
                fprintf(stderr, "FAIL: I/O completion error=%d (%s)\n",
                        -cqe.res, strerror(-cqe.res));
                return -1;
            }
            if ((unsigned)cqe.res != len) {
                fprintf(stderr, "FAIL: short I/O: expected=%u got=%d\n",
                        len, cqe.res);
                return -1;
            }
            return 0;
        }
        usleep(10);
    }

    fprintf(stderr, "FAIL: timed out waiting for CQE\n");
    return -1;
}

int main(void)
{
    struct ring r;
    int fd = -1;
    int result = 1;
    char read_buf[sizeof(INITIAL_DATA)] = {0};
    const char initial[] = INITIAL_DATA;
    const char replacement[] = REPLACEMENT_DATA;
    char verify[sizeof(REPLACEMENT_DATA)] = {0};

    printf("io_uring runtime test for Android/Linux ARM64\n");
    printf("Kernel: ");
    fflush(stdout);
    (void)system("uname -r");

    fd = open(TEST_PATH, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "FAIL: open %s: %s\n", TEST_PATH, strerror(errno));
        return 1;
    }
    if (write(fd, initial, sizeof(initial) - 1) != (ssize_t)(sizeof(initial) - 1)) {
        perror("FAIL: prepare test file");
        goto out;
    }

    if (ring_init(&r) < 0) {
        ring_cleanup(&r);
        goto out;
    }

    if (submit_io(&r, fd, IORING_OP_READ, read_buf,
                  sizeof(initial) - 1, 0, 0x1001) < 0) {
        ring_cleanup(&r);
        goto out;
    }
    if (memcmp(read_buf, initial, sizeof(initial) - 1) != 0) {
        fprintf(stderr, "FAIL: READ data mismatch\n");
        ring_cleanup(&r);
        goto out;
    }
    puts("PASS: IORING_OP_READ completed with correct data");

    if (submit_io(&r, fd, IORING_OP_WRITE, (void *)replacement,
                  sizeof(replacement) - 1, 0, 0x1002) < 0) {
        ring_cleanup(&r);
        goto out;
    }
    puts("PASS: IORING_OP_WRITE completed");

    memset(verify, 0, sizeof(verify));
    if (pread(fd, verify, sizeof(replacement) - 1, 0) !=
        (ssize_t)(sizeof(replacement) - 1) ||
        memcmp(verify, replacement, sizeof(replacement) - 1) != 0) {
        fprintf(stderr, "FAIL: WRITE verification mismatch\n");
        ring_cleanup(&r);
        goto out;
    }
    puts("PASS: WRITE data verified by pread()");
    ring_cleanup(&r);
    puts("RESULT: ALL TESTS PASSED");
    result = 0;

out:
    if (fd >= 0)
        close(fd);
    return result;
}
