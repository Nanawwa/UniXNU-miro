#!/usr/bin/env python3
"""Compile the IOStorageFamily base class the storage transports inherit from.

The storage transports (drivers/MiroUfs, drivers/VirtioMmioBlock) are subclasses of
IOBlockStorageDevice. That class is not part of the xnu source tree: it lives in Apple's separate
IOStorageFamily project, and the public headers are staged into build/storage-abi/include so the
transports can compile against them. A header alone is not enough at link time, though -- a subclass
needs the metaclass, the constructor and the vtable of its base, and a kernel is a statically linked
Mach-O with no dynamic lookup:

    Undefined symbols for architecture arm64:
      "IOBlockStorageDevice::gMetaClass", referenced from:
          NativeUfsBlock::MetaClass::MetaClass() in libMiroUfs.a(MiroUfs.o)
      "vtable for IOBlockStorageDevice", referenced from:
          NativeUfsBlock::start(IOService*) in libMiroUfs.a(MiroUfs.o)

So the base class itself is compiled from the same pinned IOStorageFamily revision the headers come
from, and linked into the kernel by board.toml [kernel] archives.

Only IOBlockStorageDevice.cpp is built. It is the class the transports derive from; the rest of
IOStorageFamily (IOMedia, the partition schemes, IOBlockStorageDriver) is a kext that the kernelcache
carries and this port does not replace. The file itself is self-contained: its only includes are
IOLib, IOUserClient and its own header, and its only external references are IOService/IOUserClient
(XNU) plus the IOStorage*Options enums, which are typedefs.

The reference object of scripts/audit-storage-interface.py is the same translation unit compiled for
the ABI audit; this builder makes an archive the linker can use.
"""
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from storage_abi_headers import PINNED_REVISION, stage

out = ROOT / 'build/storage-classes'
out.mkdir(parents=True, exist_ok=True)
source = ROOT / 'refer/IOStorageFamily'
# stage() returns build/storage-abi/include/IOKit/storage, but the class includes its own header by its
# <IOKit/storage/...> name, so the include root is build/storage-abi/include (parents[1]).
headers = stage(ROOT, source, PINNED_REVISION).parents[1]
translation_unit = source / 'IOBlockStorageDevice.cpp'
if not translation_unit.is_file():
    raise SystemExit(f'{translation_unit} is missing; the pinned IOStorageFamily revision is wrong')

recipe = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
record = json.loads(recipe.read_text().strip().rstrip(','))
destination = out / 'IOBlockStorageDevice.o'
command = []
index = 0
arguments = record['arguments']
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(translation_unit))
    elif value == '-o':
        command.extend(['-o', str(destination)])
        index += 1
    elif value in ('-MF', '-MJ'):
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W') and value not in ('-MD', '-MP'):
        command.append(value)
    index += 1
command += ['-I', str(headers),
            '-Wall', '-Wextra', '-Werror', '-Wno-unknown-warning-option',
            '-Wno-unused-parameter', '-Wno-deprecated-declarations',
            '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier']
print('+ ' + ' '.join(command), flush=True)
result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True)
(out / 'compile.log').write_text(result.stdout)
print(result.stdout, flush=True)
if result.returncode:
    raise SystemExit(f'IOBlockStorageDevice.cpp: compile failed ({result.returncode}); '
                     'the output above is the whole compiler log')

data = destination.read_bytes()
if struct.unpack_from('<4I', data) != (0xfeedfacf, 0x100000c, 0, 1):
    raise SystemExit('IOBlockStorageDevice requires ordinary ARM64 output')
offset = 32
deployment = None
for _ in range(struct.unpack_from('<I', data, 16)[0]):
    cmd, length = struct.unpack_from('<II', data, offset)
    if cmd == 0x32:
        deployment = struct.unpack_from('<II', data, offset + 8)
    offset += length
if deployment != (2, 0x110000):
    raise SystemExit(f'Unexpected IOBlockStorageDevice deployment: {deployment}')

archive = out / 'libStorageClasses.a'
archive.unlink(missing_ok=True)
# No archive is made on purpose. The transport no longer links this object: the base class is the
# kernelcache's own com.apple.iokit.IOStorageFamily kext, and scripts/build-storage-kext-bindings.py
# resolves the transport's imports to that kext's addresses. Linking this object would register a
# second IOBlockStorageDevice and OSMetaClass would refuse it. The object is still built because it
# is the reference that names the 190 virtual slots, which is what turns an import into an address.
print(f'IOBlockStorageDevice reference compiled from IOStorageFamily {PINNED_REVISION[:12]}; '
      'ordinary ARM64 / iOS 17 audited (not linked)')
