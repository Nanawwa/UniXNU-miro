#!/usr/bin/env python3
"""Build the opt-in board diagnostic against this source kernel's actual ABI."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/boot-progress'
OUT.mkdir(parents=True, exist_ok=True)
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json').read_text().strip().rstrip(','))
source = ROOT / 'kits/miro/BootProgress.c'
obj = OUT / 'BootProgress.o'
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
    elif arg == '-Xclang' and args[i+1].startswith('-W'):
        i += 1
    elif arg == '-D' and args[i+1].startswith('XNU_BOOT_PROGRESS='):
        i += 1
    elif not arg.startswith(('-W', '-DXNU_BOOT_PROGRESS=')):
        command.append(arg)
    i += 1
command += ['-DXNU_BOOT_PROGRESS=1', '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror',
            '-I', str(ROOT / '.cache/xnu-work/osfmk'),
            '-I', str(ROOT / 'uefi-sheng/Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib')]
uefi = ROOT / 'uefi-miro/Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib'
if uefi.is_dir():
    command = [part for index, part in enumerate(command)
               if not (part == '-I' and index + 1 < len(command) and 'uefi-sheng' in command[index + 1])]
    command += ['-I', str(uefi)]
result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True, timeout=90)
if result.returncode:
    raise SystemExit(result.stdout[-4000:] + result.stderr[-4000:])
(OUT / 'compile-command.json').write_text(json.dumps(command,indent=2)+'\n')
fault_source = ROOT / 'kits/miro/BootFaultProbe.c'
fault_obj = OUT / 'BootFaultProbe.o'
fault_command = [str(fault_source) if arg == str(source) else str(fault_obj) if arg == str(obj) else arg for arg in command]
subprocess.run(fault_command, cwd=record['directory'], check=True, timeout=90)
policy_source = ROOT / 'kits/miro/BootWatchdogPolicy.c'
policy_obj = OUT / 'BootWatchdogPolicy.o'
policy_record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json').read_text().strip().rstrip(','))
policy_command = []
policy_args = policy_record['arguments']
j = 0
while j < len(policy_args):
    arg = policy_args[j]
    if arg == policy_record['file']:
        policy_command.append(str(policy_source))
    elif arg in ('-o', '-MF', '-MT', '-MJ'):
        if arg == '-o': policy_command += ['-o', str(policy_obj)]
        j += 1
    elif arg == '-Xclang' and policy_args[j+1].startswith('-W'):
        j += 1
    elif not arg.startswith('-W'):
        policy_command.append(arg)
    j += 1
policy_command += ['-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror']
subprocess.run(policy_command, cwd=policy_record['directory'], check=True, timeout=90)
# The diagnostic file export (BootLog.c's sheng-diag-export) reads files through the VFS: the same BSD-side flags.
diag_source = ROOT / 'kits/miro/BootDiagExport.c'
diag_obj = OUT / 'BootDiagExport.o'
diag_command = [str(diag_source) if arg == str(policy_source) else str(diag_obj) if arg == str(policy_obj) else arg for arg in policy_command]
subprocess.run(diag_command, cwd=policy_record['directory'], check=True, timeout=90)
# The firmware file reader of the audio DSP bring-up (AdspProbe.cpp) uses the VFS too.
fw_source = ROOT / 'kits/miro/AdspFirmware.c'
fw_obj = OUT / 'AdspFirmware.o'
fw_command = [str(fw_source) if arg == str(policy_source) else str(fw_obj) if arg == str(policy_obj) else arg for arg in policy_command]
subprocess.run(fw_command, cwd=policy_record['directory'], check=True, timeout=90)

stub_source = ROOT / 'kits/miro/BootProgressStubs.c'
stub_obj = OUT / 'BootProgressStubs.o'
stub_command = [str(stub_source) if arg == str(source) else str(stub_obj) if arg == str(obj) else arg for arg in command]
subprocess.run(stub_command, cwd=record['directory'], check=True, timeout=90)
archive = OUT / 'libBootProgress.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj), str(fault_obj), str(policy_obj), str(diag_obj), str(fw_obj), str(stub_obj)], check=True, timeout=30)
(OUT / 'build-report.json').write_text(json.dumps({
    'watchdog_policy_sha256': hashlib.sha256(policy_source.read_bytes()).hexdigest(),
    'diag_sha256': hashlib.sha256(diag_source.read_bytes()).hexdigest(),
    'fault_sha256': hashlib.sha256(fault_source.read_bytes()).hexdigest(),
    'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
    'lifecycle_sha256': hashlib.sha256((source.parent / 'BootProgressLifecycle.h').read_bytes()).hexdigest(),
    'core_sha256': hashlib.sha256((source.parent / 'BootProgressCore.h').read_bytes()).hexdigest(),
    'display_sha256': hashlib.sha256((source.parent / 'BootProgressDisplay.h').read_bytes()).hexdigest(),
    'pulse_sha256': hashlib.sha256((source.parent / 'BootProgressPulse.h').read_bytes()).hexdigest(),
    'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
    'hardware_execution': False,
}, indent=2) + '\n')
print('PASS: strict source-kernel ARM64 boot diagnostic build')
