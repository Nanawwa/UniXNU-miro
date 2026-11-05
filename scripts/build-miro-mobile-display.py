#!/usr/bin/env python3
"""Publish the MobileFramebuffer prototype archive under the name the kernel link step expects.

build-arm-driver.py NativeMobileDisplay writes build/gop-framebuffer/libGopFramebuffer.a; the kernel link
step takes build/gop-framebuffer/libGopFramebuffer.late.a plus its build-report.json. This step copies the
archive and records its hash, so the link verifies exactly the object that was compiled.
"""
import hashlib
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
out = ROOT / 'build/gop-framebuffer'
source = out / 'libGopFramebuffer.a'
if not source.is_file():
    raise SystemExit(f'{source} is missing: build the GopFramebuffer component first')
target = out / 'libGopFramebuffer.late.a'
shutil.copyfile(source, target)
report = out / 'build-report.json'
report.write_text(json.dumps({
    'source_sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in (ROOT / 'drivers/GopFramebuffer/NativeMobileDisplay.cpp',
                                ROOT / 'drivers/GopFramebuffer/OriginalMobileFramebufferABI.h')
                      if p.is_file()},
    'archive_sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
}, indent=2) + '\n')
print(f'published {target.relative_to(ROOT)}')