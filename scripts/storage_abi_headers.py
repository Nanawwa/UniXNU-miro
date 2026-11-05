#!/usr/bin/env python3
"""Stage the public IOStorageFamily headers the storage transports compile against.

The storage transports (MiroUfs, VirtioMmioBlock, RamBlock) include
<IOKit/storage/IOBlockStorageDevice.h>. That header is not part of the xnu source tree: the storage classes
live in Apple's separate IOStorageFamily project, and the target kernelcache carries the original kext. The
headers here are the public Darwin 23 originals, staged where the archive builders already look
(build/storage-abi/include), so a transport is compiled against the class layout the prelinked original
kext exports rather than against a locally reinvented one.

This is deliberately not part of scripts/audit-storage-interface.py. That script is a qualification
instrument: it also compiles a reference object, reads the vtable of the prelinked original kext and checks
the dispatch shape, which needs build/native-prelink/native-apfs-load-positive.macho -- an artifact of the
prelink step that does not exist while the archives are built. It uses stage() so both share one pinned
IOStorageFamily revision.
"""
import shutil
import subprocess
from pathlib import Path

# The tag the qualified original kext and these headers both come from. audit-storage-interface.py records
# the same revision; feeds.toml pins it for the fetch.
PINNED_REVISION = 'e45fbe30a7879e0b16e26dddb78538f1b00e0bff'


def stage(root, source=None, expected=PINNED_REVISION, sdk=None):
    """Copy the family headers and TargetConditionals.h into build/storage-abi/include; return that directory."""
    root = Path(root)
    source = Path(source) if source is not None else root / 'refer/IOStorageFamily'
    if not source.is_dir():
        raise SystemExit(f'{source} missing: it is the IOStorageFamily source of feeds.toml, run "unixnu fetch"')
    if expected is not None:
        revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
        if revision != expected:
            raise SystemExit(f'{source} is at {revision[:12]}, not the qualified IOStorageFamily {expected[:12]}')
    out = root / 'build/storage-abi'
    headers = out / 'include/IOKit/storage'
    headers.mkdir(parents=True, exist_ok=True)
    staged = 0
    for path in sorted(source.glob('*.h')):
        target = headers / path.name
        # Rewrite only a changed file, so an unchanged tree keeps its timestamps (and make its order).
        if not target.exists() or target.read_bytes() != path.read_bytes():
            shutil.copyfile(path, target)
        staged += 1
    if not staged:
        raise SystemExit(f'{source} carries no IOStorageFamily header; the pinned revision is wrong')
    # TargetConditionals.h selects the platform in Apple's headers and is absent from the source-kernel
    # SDK the compile records point at, so it comes from the macOS SDK of the build host.
    sdk = Path(sdk) if sdk is not None else Path(subprocess.check_output(
        ['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip())
    conditions = sdk / 'usr/include/TargetConditionals.h'
    if not conditions.is_file():
        raise SystemExit(f'{conditions} missing: the macOS SDK is needed for the platform-selection header')
    target = out / 'include/TargetConditionals.h'
    if not target.exists() or target.read_bytes() != conditions.read_bytes():
        shutil.copyfile(conditions, target)
    return headers


if __name__ == '__main__':
    # kernel.py runs this before the archive builders of a board with a storage transport.
    here = Path(__file__).resolve().parents[1]
    staged = stage(here)
    print(f'storage ABI headers: {len(list(staged.glob("*.h")))} IOStorageFamily headers in {staged}')
