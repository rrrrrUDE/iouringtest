# Maximum io_uring Functional Validation for Android

A functional runtime test for an Android/Linux ARM64 kernel. It creates an io_uring instance, maps SQ/CQ/SQEs, submits asynchronous READ/WRITE operations, repeats submissions, tests FSYNC completion, probes supported operations, and verifies file data.

GitHub Actions cross-compiles a standalone AArch64 Android executable; it does not build or modify the kernel.

## Download

1. Open the repository's **Actions** tab.
2. Open a successful **Build io_uring test** run.
3. Download the `iouringtest-android-arm64` artifact ZIP and extract `iouringtest-arm64`.

A push to `main` or manual **Run workflow** starts a build.

## Run on the phone

From a root-capable shell, adjust the source path if needed:

```sh
su
cp /sdcard/Download/iouringtest-arm64 /data/local/tmp/iouringtest
chmod 755 /data/local/tmp/iouringtest
/data/local/tmp/iouringtest
```

## Tests

The program validates:

1. `io_uring_setup`
2. SQ/CQ ring and SQE memory mappings
3. `io_uring_enter`
4. asynchronous `IORING_OP_WRITE`
5. asynchronous `IORING_OP_READ`
6. READ data integrity
7. repeated WRITE/READ submissions
8. `IORING_OP_FSYNC`
9. `io_uring_register(IORING_REGISTER_PROBE)`
10. final file data verification and cleanup

Each submitted I/O waits for its CQE with a **10-second timeout**. A timeout is treated as a failure rather than waiting indefinitely.

## Interpret results

- `RESULT: ALL FUNCTIONAL TESTS PASSED`: all functional checks completed successfully.
- `RESULT: FAILED - io_uring operation timed out`: an expected completion did not arrive within 10 seconds.
- `ENOSYS`: the syscall is unavailable or not implemented.
- `EPERM`: a security policy or syscall filter may be blocking it.
- `EINVAL`: the syscall rejected the request; this alone does not prove io_uring is unavailable.
- Other errors: check the printed errno and the kernel log.

Run from a root shell to reduce permission-related ambiguity. Android seccomp, SELinux, or vendor policy can make a test under an app differ from one run in a root shell.

## Kernel log

For kernel-side diagnosis, capture logs before and after the test:

```sh
dmesg > /sdcard/iouring-before.log
/data/local/tmp/iouringtest
dmesg > /sdcard/iouring-after.log
```

Compare the logs for `io_uring`, `BUG:`, `Oops:`, `WARNING:`, and memory/protection faults.

## Scope

This is an aggressive functional smoke test, not a benchmark or exhaustive conformance suite. It does not test every io_uring opcode, registered buffers/files, SQPOLL, fixed resources, advanced multishot operations, or performance.
