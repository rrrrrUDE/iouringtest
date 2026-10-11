#define _GNU_SOURCE
#include <errno.h>
#include <linux/io_uring.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif

int main(void)
{
    struct io_uring_params params;
    int fd;

    memset(&params, 0, sizeof(params));
    printf("Minimal io_uring validation (Android/Linux ARM64)\n");
    printf("Kernel: ");
    fflush(stdout);
    (void)system("uname -r");

    fd = (int)syscall(__NR_io_uring_setup, 2, &params);
    if (fd < 0) {
        int err = errno;
        fprintf(stderr, "FAIL: io_uring_setup: errno=%d (%s)\n",
                err, strerror(err));
        if (err == ENOSYS)
            fprintf(stderr, "HINT: io_uring_setup syscall is unavailable.\n");
        else if (err == EPERM)
            fprintf(stderr, "HINT: syscall may be blocked by policy/seccomp.\n");
        else if (err == EINVAL)
            fprintf(stderr, "HINT: syscall exists but rejected the setup parameters.\n");
        return 1;
    }

    printf("PASS: io_uring_setup succeeded (fd=%d, SQ=%u, CQ=%u, features=0x%x)\n",
           fd, params.sq_entries, params.cq_entries, params.features);

    if (close(fd) != 0) {
        fprintf(stderr, "FAIL: close ring fd: errno=%d (%s)\n",
                errno, strerror(errno));
        return 1;
    }

    puts("PASS: io_uring instance closed cleanly");
    puts("RESULT: MINIMAL VALIDATION PASSED");
    return 0;
}
