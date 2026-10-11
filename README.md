# io_uring Test for Android

A small runtime test for checking whether an Android/Linux kernel can create an io_uring instance, map its shared rings, submit `IORING_OP_READ` and `IORING_OP_WRITE`, and receive successful completion queue entries.

This project does not modify or build a kernel. GitHub Actions cross-compiles a standalone AArch64 Android test binary.

## Download the binary

1. Open the repository's **Actions** tab.
2. Select **Build io_uring test** and open a successful run.
3. Download the `iouringtest-android-arm64` artifact ZIP.
4. Extract `iouringtest-arm64` on your phone.

A new push to `main` or a manual **Run workflow** also starts a build.

## Run on the Android phone

Move the extracted executable to `/data/local/tmp` using a root-capable shell. For example, if the browser saved it to Downloads:

```sh
su
cp /sdcard/Download/iouringtest-arm64 /data/local/tmp/iouringtest
chmod 755 /data/local/tmp/iouringtest
/data/local/tmp/iouringtest
```

Adjust the source path if your browser uses a different download directory.

The test creates `/data/local/tmp/iouringtest-data.txt`, then performs an io_uring read and write and verifies the resulting data. It does not intentionally modify any existing user file.

## Interpret results

- `RESULT: ALL TESTS PASSED`: setup, ring mappings, READ, WRITE, and completion checks passed.
- `Function not implemented` (`ENOSYS`): syscall is unavailable or not implemented.
- `Operation not permitted` (`EPERM`): a security policy or syscall filter may be blocking it.
- `Invalid argument` (`EINVAL`): inspect the kernel implementation and dmesg; this alone does not prove the syscall is absent.
- Other failures: check the printed errno and kernel log.

Run it from a shell with sufficient permissions. Android seccomp, SELinux, or vendor policy can make a test under an app differ from one run in a root shell.

## Scope

This is a focused smoke test for basic file READ/WRITE on the test file. It does not validate every io_uring opcode, registered buffers/files, SQPOLL, or performance.
