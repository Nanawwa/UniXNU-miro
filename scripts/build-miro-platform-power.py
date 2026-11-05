#!/usr/bin/env python3
"""Build the miro original-PMGR instrument.

The instrument is the sheng ApplePMGR subclass that publishes the ApplePMGRNub
service AMFI asks for.  sheng derived its two binding tables from an owner-root
prelink candidate that is not part of this workspace; the addresses below are
the same functions in the same kernelcache and the same prelink slide, so they
are fixed here.  docs/known-values.md records the instruction comparison that
qualifies them.
"""
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/miro-platform-power'
OUT.mkdir(parents=True, exist_ok=True)

header = '''// Generated exact storage/slot ABI; private fields stay opaque.
#pragma once
#include <IOKit/IOPMGR.h>
class ApplePMGR : public IOPMGR {
 OSDeclareAbstractStructors(ApplePMGR)
 unsigned char originalPrivate[0x4b8a8-sizeof(IOPMGR)];
public:
'''
guards = ''
for i in range(177, 340):
    if i == 320:
        header += ' virtual uint32_t readAon();\n'
    elif i == 321:
        header += ' virtual void writeAon(uint64_t);\n'
    else:
        header += f' virtual void opaque{i}() = 0;\n'
        guards += f' void opaque{i}() override {{ unavailable({i}); }}\n'
header += '};\nstatic_assert(sizeof(ApplePMGR)==0x4b8a8,"Original provider storage");\n'
(OUT / 'OriginalPMGRABI.h').write_text(header)
(OUT / 'GuardedPMGRSlots.inc').write_text(guards)

record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
device = next(iter((ROOT / 'drivers').glob('Miro*'))).resolve().parents[1]
source = device / 'drivers/MiroPlatformPower/MiroPlatformPower.cpp'
obj = OUT / 'MiroPlatformPower.o'
command, index = [], 0
arguments = record['arguments']
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(source))
    elif value == '-o':
        command += ['-o', str(obj)]
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        command.append(value)
    index += 1
command += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator',
            '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
            '-Wno-unnecessary-virtual-specifier', '-I', str(OUT), '-I', str(ROOT / '.cache/xnu-work/osfmk')]
env = dict(os.environ)
env['CCC_OVERRIDE_OPTIONS'] = '# +-Wno-missing-designated-field-initializers +-Wno-unused-but-set-global'
result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True, env=env)
(OUT / 'compile.log').write_text(result.stdout + result.stderr)
if result.returncode:
    raise SystemExit(result.stderr[-4500:])
raw = obj.read_bytes()
if struct.unpack_from('<4I', raw) != (0xfeedfacf, 0x100000c, 0, 1):
    raise SystemExit('PlatformPower requires ordinary ARM64 output')

bindings = {
    '__ZN9ApplePMGRC2EPK11OSMetaClass': 0xfffffff0049f003c,
    '__ZN9ApplePMGRD2Ev': 0xfffffff0049f019c,
    '__ZN9ApplePMGR10gMetaClassE': 0xfffffff006adf490,
    '__ZN9ApplePMGR7readAonEv': 0xfffffff004a20fbc,
    '__ZN9ApplePMGR8writeAonEy': 0xfffffff004a20fc4,
    '_sheng_original_pmgr_nub_create': 0xfffffff004a22568,
    '_sheng_original_pmgr_nub_read': 0xfffffff004a24adc,
    '_sheng_original_pmgr_nub_write': 0xfffffff004a230ec,
}
asm = OUT / 'original-bindings.S'
asm.write_text(''.join(f'.globl {name}\n.set {name}, {va:#x}\n' for name, va in bindings.items()))
bindings_obj = OUT / 'original-bindings.o'
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-c', str(asm), '-o', str(bindings_obj)], check=True)


# sheng removes the mod_init functor section and globalizes the static initializer with objcopy;
# llvm-objcopy cannot edit Mach-O sections/symbols, so the object is patched directly.
import struct
blob = bytearray(obj.read_bytes())
magic, _, _, _, ncmds, _, _, _ = struct.unpack_from('<IiiIIIII', blob, 0)
if magic != 0xfeedfacf:
    raise SystemExit('PlatformPower is not a 64-bit Mach-O')
off, symtab = 32, None
for _ in range(ncmds):
    cmd, cmdsize = struct.unpack_from('<II', blob, off)
    if cmd == 0x19:
        nsects = struct.unpack_from('<I', blob, off + 64)[0]
        sec = off + 72
        for _ in range(nsects):
            if blob[sec:sec+16].split(b'\0')[0] == b'__mod_init_func':
                struct.pack_into('<Q', blob, sec + 40, 0)
            sec += 80
    elif cmd == 0x2:
        symtab = struct.unpack_from('<IIII', blob, off + 8)
    off += cmdsize
if symtab is None:
    raise SystemExit('PlatformPower object has no symbol table')
symoff, nsyms, stroff, _ = symtab
for index in range(nsyms):
    at = symoff + index * 16
    strx, ntype = struct.unpack_from('<IB', blob, at)
    name = blob[stroff+strx:blob.index(b'\0', stroff + strx)].decode()
    if name == '__GLOBAL__sub_I_MiroPlatformPower.cpp':
        blob[at+4] = ntype | 0x01
        break
else:
    raise SystemExit('static initializer symbol not found')
late = OUT / 'MiroPlatformPower.late.o'
late.write_bytes(blob)
archive = OUT / 'libMiroPlatformPower.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(late), str(bindings_obj)], check=True)
print(f'miro platform power: {archive.relative_to(ROOT)} ({archive.stat().st_size} bytes)')