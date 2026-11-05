# Build evidence

These files are copies of the reports written by the build commands in
[porting.toml](../porting.toml). They describe the host build only; no hardware
result is recorded here.

| File | Source command | Contents |
| --- | --- | --- |
| `kernel-build.json` | `./unixnu build kernel`, `build firmware`, `build payload`, `bootimg` | Host, toolchain, commands, firmware/kernel/payload/boot hashes, prelink component count |
| `payload.json` | `./unixnu build payload -D ../UniXNU-miro` | Payload volume, kernel and ADT hashes, effective kernel arguments |
| `boot.json` | `./unixnu bootimg -D ../UniXNU-miro` | Android header v4, ramdisk size, firmware and DTB hashes |

The build used the cached 21H461 kernelcache and a placeholder platform seed.
The image was not flashed.