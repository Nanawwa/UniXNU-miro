#!/usr/bin/env python3
"""Build the miro UFS diagnostic log writer (kits/miro/BootLog.c) against this kernel's own compile record."""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/miro-boot-log'
OUT.mkdir(parents=True, exist_ok=True)
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json').read_text().strip().rstrip(','))
source = ROOT / 'kits/miro/BootLog.c'
obj = OUT / 'BootLog.o'
command = []
args = record['arguments']
i = 0
while i < len(args):
    arg = args[i]
    if arg == record['file']:
        command.append(str(source))
    elif arg in ('-o', '-MF', '-MT', '-MJ'):
        if arg == '-o':
            command += ['-o', str(obj)]
        i += 1
    elif arg == '-Xclang' and args[i + 1].startswith('-W'):
        i += 1
    elif arg == '-D' and args[i + 1].startswith('XNU_BOOT_PROGRESS='):
        i += 1
    elif not arg.startswith(('-W', '-DXNU_BOOT_PROGRESS=')):
        command.append(arg)
    i += 1
command += ['-mgeneral-regs-only', '-I', str(ROOT / '.cache/xnu-work/osfmk')]
result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True, timeout=120)
if result.returncode:
    raise SystemExit(result.stdout[-4000:] + result.stderr[-4000:])
proc_source = ROOT / 'kits/miro/BootProcessProbe.c'
proc_obj = OUT / 'BootProcessProbe.o'
proc_command = [str(proc_source) if arg == str(source) else str(proc_obj) if arg == str(obj) else arg for arg in command]
result = subprocess.run(proc_command, cwd=record['directory'], capture_output=True, text=True, timeout=120)
if result.returncode:
    raise SystemExit(result.stdout[-4000:] + result.stderr[-4000:])
# The fault probe must be in the archive: miro_record_crash() calls
# boot_progress_exit_fault(), which only this unit defines.
fault_source = ROOT / 'kits/miro/BootFaultProbe.c'
fault_obj = OUT / 'BootFaultProbe.o'
fault_command = [str(fault_source) if arg == str(source) else str(fault_obj) if arg == str(obj) else arg for arg in command]
result = subprocess.run(fault_command, cwd=record['directory'], capture_output=True, text=True, timeout=120)
if result.returncode:
    raise SystemExit(result.stdout[-4000:] + result.stderr[-4000:])
archive = OUT / 'libMiroBootLog.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj), str(proc_obj), str(fault_obj)], check=True)
(OUT / 'build-report.json').write_text(json.dumps({
    'source_sha256': __import__('hashlib').sha256(source.read_bytes()).hexdigest(),
    'archive_sha256': __import__('hashlib').sha256(archive.read_bytes()).hexdigest(),
}, indent=2) + '\n')
print('PASS: miro UFS diagnostic log writer built')