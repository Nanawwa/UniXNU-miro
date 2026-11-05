#!/usr/bin/env python3
"""Build the miro RAM Data transport (NativeRamBlock) reusing MiroUfs's audited storage bindings."""
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/ram-block'
OUT.mkdir(parents=True, exist_ok=True)
args = json.loads((ROOT / 'build/miro-ufs/compile-command.json').read_text())
source_entry = next(a for a in args if a.endswith('/drivers/MiroUfs/MiroUfs.cpp'))
driver_dir = str(Path(source_entry).parent)
ram_dir = str(Path(driver_dir).parent / 'RamBlock')
source = Path(ram_dir) / 'NativeRamBlock.cpp'
args = [ram_dir if a == driver_dir else (str(source) if a == source_entry else a) for a in args]
obj = OUT / 'NativeRamBlock.o'
args = [a for a in args]
args.insert(len(args) - 0, '-I')
args.append(ram_dir)
index = args.index('-o')
args[index + 1] = str(obj)
env = dict(os.environ)
env['CCC_OVERRIDE_OPTIONS'] = '# +-Wno-missing-designated-field-initializers +-Wno-unused-but-set-global'
cwd = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT'
result = subprocess.run(args, capture_output=True, text=True, env=env, cwd=cwd)
(OUT / 'compile.log').write_text(result.stdout + result.stderr)
if result.returncode:
    raise SystemExit(result.stderr[-4500:])

stub_source = OUT / 'manifest-stub.cpp'
stub_source.write_text("""// Bring-up: this port has no signed owner policy, so the boot-manifest digest
// is a fixed zero digest instead of a per-device manifest hash.
extern "C" int native_verified_block_manifest_sha384(int, const char *, unsigned char digest[48])
{
  for (int i = 0; i < 48; i++) { digest[i] = 0; }
  return 1;
}
""")
stub_obj = OUT / 'manifest-stub.o'
compiler = ['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-c', str(stub_source), '-o', str(stub_obj)]
result = subprocess.run(compiler, capture_output=True, text=True)
if result.returncode:
    raise SystemExit(result.stderr[-2000:])
archive = OUT / 'libRamBlock.a'
archive.unlink(missing_ok=True)
# The IOBlockStorageDevice aliases live in MiroUfs's original-bindings.o; a second copy would duplicate them.
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj), str(stub_obj)], check=True)
print(f'miro ram block: {archive.relative_to(ROOT)} ({archive.stat().st_size} bytes)')