# Minimal io_uring Validation for Android

A minimal runtime check for an Android/Linux ARM64 kernel: call `io_uring_setup(2, ...)` and close the returned file descriptor. It does **not** map the rings or submit I/O.

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

## Interpret results

- `RESULT: MINIMAL VALIDATION PASSED`: the kernel accepted `io_uring_setup` and the instance was closed cleanly.
- `ENOSYS` / `Function not implemented`: the syscall is unavailable or not implemented.
- `EPERM` / `Operation not permitted`: a security policy or syscall filter may be blocking it.
- `EINVAL`: the syscall returned an invalid-argument error; this alone does not prove the syscall is absent.

Run from a root shell to reduce permission-related ambiguity. A pass only confirms that an io_uring instance can be created and closed. It does **not** verify ring memory mappings, `io_uring_enter`, READ/WRITE operations, completion events, stability, or performance.
