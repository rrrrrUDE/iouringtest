#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter 426
#endif
#ifndef __NR_io_uring_register
#define __NR_io_uring_register 427
#endif

#define N 8
#define PATH "/data/local/tmp/iouringtest-data.bin"
#define IO_TIMEOUT_MS 10000
#define TEST_LOOPS 8

struct ring {
    int fd;
    struct io_uring_params p;
    void *sq, *cq;
    size_t sqsz, cqsz, sqesz;
    struct io_uring_sqe *sqes;
    unsigned *sq_head, *sq_tail, *sq_mask, *sq_entries, *sq_array;
    unsigned *cq_head, *cq_tail, *cq_mask;
    struct io_uring_cqe *cqes;
};

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void cleanup(struct ring *r)
{
    if (r->sqes) munmap(r->sqes, r->sqesz);
    if (r->cq && r->cq != r->sq) munmap(r->cq, r->cqsz);
    if (r->sq) munmap(r->sq, r->sqsz);
    if (r->fd >= 0) close(r->fd);
    memset(r, 0, sizeof(*r));
    r->fd = -1;
}

static int setup_ring(struct ring *r)
{
    memset(r, 0, sizeof(*r));
    r->fd = -1;
    r->fd = (int)syscall(__NR_io_uring_setup, N, &r->p);
    if (r->fd < 0) {
        fprintf(stderr, "FAIL: io_uring_setup errno=%d (%s)\n",
                errno, strerror(errno));
        return -1;
    }

    r->sqsz = r->p.sq_off.array + r->p.sq_entries * sizeof(unsigned);
    r->cqsz = r->p.cq_off.cqes +
              r->p.cq_entries * sizeof(struct io_uring_cqe);

    if (r->p.features & IORING_FEAT_SINGLE_MMAP) {
        if (r->cqsz > r->sqsz)
            r->sqsz = r->cqsz;
        r->cqsz = r->sqsz;
    }

    r->sq = mmap(NULL, r->sqsz, PROT_READ | PROT_WRITE,
                 MAP_SHARED, r->fd, IORING_OFF_SQ_RING);
    if (r->sq == MAP_FAILED) {
        r->sq = NULL;
        perror("FAIL: mmap SQ");
        return -1;
    }

    if (r->p.features & IORING_FEAT_SINGLE_MMAP) {
        r->cq = r->sq;
    } else {
        r->cq = mmap(NULL, r->cqsz, PROT_READ | PROT_WRITE,
                     MAP_SHARED, r->fd, IORING_OFF_CQ_RING);
        if (r->cq == MAP_FAILED) {
            r->cq = NULL;
            perror("FAIL: mmap CQ");
            return -1;
        }
    }

    r->sqesz = r->p.sq_entries * sizeof(struct io_uring_sqe);
    r->sqes = mmap(NULL, r->sqesz, PROT_READ | PROT_WRITE,
                   MAP_SHARED, r->fd, IORING_OFF_SQES);
    if (r->sqes == MAP_FAILED) {
        r->sqes = NULL;
        perror("FAIL: mmap SQEs");
        return -1;
    }

    r->sq_head = (unsigned *)((char *)r->sq + r->p.sq_off.head);
    r->sq_tail = (unsigned *)((char *)r->sq + r->p.sq_off.tail);
    r->sq_mask = (unsigned *)((char *)r->sq + r->p.sq_off.ring_mask);
    r->sq_entries = (unsigned *)((char *)r->sq + r->p.sq_off.ring_entries);
    r->sq_array = (unsigned *)((char *)r->sq + r->p.sq_off.array);

    r->cq_head = (unsigned *)((char *)r->cq + r->p.cq_off.head);
    r->cq_tail = (unsigned *)((char *)r->cq + r->p.cq_off.tail);
    r->cq_mask = (unsigned *)((char *)r->cq + r->p.cq_off.ring_mask);
    r->cqes = (struct io_uring_cqe *)((char *)r->cq + r->p.cq_off.cqes);

    printf("PASS: setup SQ=%u CQ=%u features=0x%x\n",
           r->p.sq_entries, r->p.cq_entries, r->p.features);
    return 0;
}

static int submit(struct ring *r, int fd, int opcode, void *buf,
                  unsigned len, uint64_t off, uint64_t ud)
{
    unsigned head = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
    unsigned tail = __atomic_load_n(r->sq_tail, __ATOMIC_RELAXED);

    if (tail - head >= *r->sq_entries) {
        errno = EBUSY;
        return -1;
    }

    unsigned i = tail & *r->sq_mask;
    struct io_uring_sqe *s = &r->sqes[i];

    memset(s, 0, sizeof(*s));
    s->opcode = opcode;
    s->fd = fd;
    s->off = off;
    s->addr = (uint64_t)(uintptr_t)buf;
    s->len = len;
    s->user_data = ud;

    r->sq_array[i] = i;
    __atomic_store_n(r->sq_tail, tail + 1, __ATOMIC_RELEASE);

    if (syscall(__NR_io_uring_enter, r->fd, 1, 1,
                IORING_ENTER_GETEVENTS, NULL, 0) < 0)
        return -1;

    long long deadline = now_ms() + IO_TIMEOUT_MS;

    for (;;) {
        unsigned h = __atomic_load_n(r->cq_head, __ATOMIC_RELAXED);
        unsigned t = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);

        if (h != t) {
            struct io_uring_cqe c = r->cqes[h & *r->cq_mask];

            __atomic_store_n(r->cq_head, h + 1, __ATOMIC_RELEASE);

            if (c.user_data != ud) {
                errno = EPROTO;
                return -1;
            }
            if (c.res < 0) {
                errno = -c.res;
                return -1;
            }
            if ((unsigned)c.res != len) {
                errno = EIO;
                return -1;
            }
            return 0;
        }

        if (now_ms() >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }

        sched_yield();
    }
}

static int run_tests(struct ring *r)
{
    int fd = -1;
    char w[4096], rr[4096];
    memset(w, 0x5a, sizeof(w));
    memset(rr, 0, sizeof(rr));

    fd = open(PATH, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        perror("FAIL: open test file");
        return -1;
    }

    puts("TEST 1: asynchronous WRITE");
    if (submit(r, fd, IORING_OP_WRITE, w, sizeof(w), 0, 1) < 0) {
        perror("FAIL: WRITE");
        goto fail;
    }
    puts("PASS: async WRITE completion");

    puts("TEST 2: asynchronous READ");
    if (submit(r, fd, IORING_OP_READ, rr, sizeof(rr), 0, 2) < 0) {
        perror("FAIL: READ");
        goto fail;
    }
    if (memcmp(w, rr, sizeof(w)) != 0) {
        errno = EIO;
        perror("FAIL: READ data mismatch");
        goto fail;
    }
    puts("PASS: async READ completion and data");

    puts("TEST 3: repeated submissions");
    for (int i = 0; i < TEST_LOOPS; ++i) {
        memset(rr, 0, sizeof(rr));
        if (submit(r, fd, IORING_OP_WRITE, w, sizeof(w),
                   (uint64_t)i * sizeof(w), 100 + i) < 0) {
            perror("FAIL: repeated WRITE");
            goto fail;
        }
        if (submit(r, fd, IORING_OP_READ, rr, sizeof(rr),
                   (uint64_t)i * sizeof(w), 200 + i) < 0) {
            perror("FAIL: repeated READ");
            goto fail;
        }
        if (memcmp(w, rr, sizeof(w)) != 0) {
            errno = EIO;
            perror("FAIL: repeated READ data");
            goto fail;
        }
    }
    printf("PASS: %d repeated WRITE/READ cycles\n", TEST_LOOPS);

    puts("TEST 4: IORING_OP_FSYNC");
    if (submit(r, fd, IORING_OP_FSYNC, NULL, 0, 0, 300) < 0) {
        perror("FAIL: FSYNC");
        goto fail;
    }
    puts("PASS: FSYNC completion");

    puts("TEST 5: io_uring_register(PROBE)");
    {
        size_t sz = 4096;
        struct io_uring_probe *probe = calloc(1, sz);
        if (!probe) {
            perror("FAIL: calloc probe");
            goto fail;
        }

        probe->ops_len = 128;
        int n = syscall(__NR_io_uring_register, r->fd,
                        IORING_REGISTER_PROBE, probe, 128);
        if (n < 0) {
            printf("INFO: PROBE unavailable: errno=%d (%s)\n",
                   errno, strerror(errno));
        } else {
            printf("PASS: PROBE returned %d, last_op=%u\n",
                   n, probe->last_op);
        }
        free(probe);
    }

    puts("TEST 6: final file verification");
    if (fsync(fd) < 0) {
        perror("FAIL: final fsync");
        goto fail;
    }

    for (size_t i = 0; i < sizeof(w); ++i)
        rr[i] = 0;
    if (pread(fd, rr, sizeof(rr), 0) != (ssize_t)sizeof(rr)) {
        perror("FAIL: final pread");
        goto fail;
    }
    if (memcmp(w, rr, sizeof(w)) != 0) {
        errno = EIO;
        perror("FAIL: final data verification");
        goto fail;
    }
    puts("PASS: final file data verification");

    close(fd);
    unlink(PATH);
    return 0;

fail:
    close(fd);
    unlink(PATH);
    return -1;
}

int main(void)
{
    struct ring r;
    long long start, elapsed;
    int ret;

    memset(&r, 0, sizeof(r));
    r.fd = -1;

    puts("=== MAXIMUM io_uring FUNCTIONAL VALIDATION ===");
    printf("Kernel: ");
    fflush(stdout);
    (void)system("uname -r");

    start = now_ms();

    if (setup_ring(&r) < 0) {
        cleanup(&r);
        return 1;
    }

    ret = run_tests(&r);
    elapsed = now_ms() - start;

    cleanup(&r);

    printf("Total test time: %lld ms\n", elapsed);

    if (ret == 0) {
        puts("RESULT: ALL FUNCTIONAL TESTS PASSED");
        return 0;
    }

    if (errno == ETIMEDOUT)
        puts("RESULT: FAILED - io_uring operation timed out");
    else
        puts("RESULT: FUNCTIONAL VALIDATION FAILED");

    return 1;
}
