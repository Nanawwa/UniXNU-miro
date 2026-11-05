#!/usr/bin/env python3
"""Check that the kernel config, the archive builders and their products agree.

The kernel step of this port is expensive and needs macOS, so the parts that can be checked anywhere are
checked here instead:

  1. every CONFIG_XNU_* this defconfig turns on has a builder in board.toml [kernel] archives, and every
     switch it turns off has none (build-xnu-4k-baseline.sh links each enabled archive with -force_load
     and fails on a missing one);
  2. the builders put their object in the directory the kernel script reads, so the archive it tests for
     is the archive that gets made;
  3. build-arm-driver.py compiles MiroUfs without NATIVE_VERIFIED_BLOCK unless --verified-root is
     given. Verified block mode refuses to publish the device without an owner-signed policy bundle
     ("policy count invalid"), and this port has no bundle, so the default must be the plain read path.
  4. the storage binding builder still takes the prelink slide and only rebases kext-owned slots.

Check 3 runs the real builder against a synthetic compile record, so it exercises the argument guards and
the emitted command line without Xcode. Exit status is 0 when every check passes.
"""
from pathlib import Path
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib

HERE = Path(__file__).resolve()
DEVICE = HERE.parents[2]

# CONFIG_XNU_<NAME> -> the archive build-xnu-4k-baseline.sh demands when it is 1, and the builder that makes it.
ARCHIVES = {
    'XNU_SOURCE_CORECRYPTO14': ('build/corecrypto-2023/libcorecrypto_kernel_noasm-namespaced.a',
                                ('script', 'build-corecrypto-14.py')),
    'XNU_SOURCE_PTHREAD': ('build/libpthread-kernel/libpthread_kernel.a',
                           ('script', 'build-libpthread-kernel.py')),
    'XNU_ARM_DT_PLATFORM': ('build/armdt-platform/libArmDTPlatform.a', ('component', 'ArmDTPlatform')),
    'XNU_ARMCPU': ('build/armcpu/libArmCpu.a', ('component', 'ArmCpu')),
    'XNU_NATIVE_UFS': ('build/miro-ufs/libMiroUfs.a', ('component', 'MiroUfs')),
    'XNU_NATIVE_AES': ('build/software-aes/libSoftwareAES.a', ('component', 'SoftwareAES')),
    'XNU_BOOT_PROGRESS': ('build/boot-progress/libBootProgress.a', ('script', 'build-boot-progress.py')),
    'XNU_NATIVE_LEGACY_DISPLAY': ('build/native-legacy-display/libNativeLegacyDisplay.late.a',
                                  ('script', 'build-native-legacy-display.py')),
}

failures = []
passes = []

# (switch, builders the switch requires together, archive the kernel script links, switch that makes
# them unnecessary)
#
# The storage transport derives from IOBlockStorageDevice, a class of Apple's separate IOStorageFamily
# project that the kernelcache already carries as com.apple.iokit.IOStorageFamily. The base class must
# NOT be compiled and linked: a second class of that name makes OSMetaClass panic ("class
# IOBlockStorageDevice is a duplicate; kext __kernel__ already has a class by that name").
#
# So the transport needs two things instead of a linked base class:
#   - build-storage-classes.py compiles the pinned reference object, which names the 190 dispatch slots;
#   - build-storage-kext-bindings.py rebinds the transport's imports to the prelinked kext's own
#     addresses, and rebuilds libMiroUfs.a around the bound object.
# Both are needed whenever the transport carries its block-device half.
REQUIRED_TOGETHER = (
    ('XNU_NATIVE_UFS', (('script', 'build-storage-classes.py'),
                        ('script', 'build-storage-kext-bindings.py')),
     'build/miro-ufs/libMiroUfs.a'),
)


def check(ok, message):
    (passes if ok else failures).append(message)
    return ok


def defconfig_switches(path):
    switches = {}
    for line in path.read_text().splitlines():
        match = re.match(r'\s*CONFIG_(\w+)\s*=\s*(\S+)', line)
        if match:
            switches[match.group(1)] = match.group(2).strip('"')
    return switches


def check_archives(device, tree):
    board_path = device / 'board.toml'
    if not check(board_path.is_file(), f'{board_path} exists'):
        return
    board = tomllib.load(board_path.open('rb'))
    entries = board.get('kernel', {}).get('archives')
    if not check(isinstance(entries, list) and entries, 'board.toml: [kernel] archives is a non-empty list'):
        return
    selected, scripts = set(), set()
    for entry in entries:
        parts = entry.split()
        if parts[0].startswith('component:'):
            selected.add(parts[0].split(':', 1)[1])
        else:
            scripts.add(parts[0])

    switches = defconfig_switches(device / 'configs' / board['kernel']['defconfig'])
    script = (tree / 'scripts/build-xnu-4k-baseline.sh').read_text() if tree else ''
    for name, (archive, builder) in ARCHIVES.items():
        value = switches.get(name, 'n')
        enabled = value == 'y'
        kind, identifier = builder
        has_builder = identifier in (selected if kind == 'component' else scripts)
        if enabled:
            check(has_builder, f'{name}=y selects {kind} {identifier}')
            if script:
                overrides = {value.replace('{tree}/', '') for key, value in switches.items()
                             if key.endswith('_ARCHIVE')}
                check(archive in script or archive in overrides,
                      f'{name} archive {archive} is the one the kernel script reads')
        else:
            check(not has_builder, f'{name}=n selects no {kind} {identifier}')

    # Some archives exist only to supply symbols a selected one needs. The storage transport derives
    # from IOBlockStorageDevice, a class of Apple's separate IOStorageFamily project, so the kernel
    # must carry the pinned reference shape and the kext bindings whenever the transport is linked.
    for switch, builders, archive in REQUIRED_TOGETHER:
        needed = switches.get(switch, 'n') == 'y'
        for kind, identifier in builders:
            present = identifier in (selected if kind == 'component' else scripts)
            check(present == needed,
                  f'{switch}={"y" if needed else "n"} lists {kind} {identifier} '
                  f'{"and needs it" if needed else "only when it does not need it"}')
        if needed and script:
            overrides = {value.replace('{tree}/', '') for key, value in switches.items()
                         if key.endswith('_ARCHIVE')}
            check(archive in script or archive in overrides, f'{switch}=y links {archive}')


def check_products(device, tree):
    """Each selected kernel component must build the archive the kernel script reads."""
    catalog_path = find_catalog(device, tree)
    if not check(catalog_path is not None, 'components.toml is available for the product check'):
        return
    specs = {entry['id']: entry for entry in tomllib.loads(catalog_path.read_text())['component']}
    script = (tree / 'scripts/build-xnu-4k-baseline.sh').read_text() if tree else ''
    board = tomllib.load((device / 'board.toml').open('rb'))
    switches = defconfig_switches(device / 'configs' / board['kernel']['defconfig'])
    overrides = {value.replace('{tree}/', '') for key, value in switches.items() if key.endswith('_ARCHIVE')}
    expected = {
        'ArmDTPlatform': 'build/armdt-platform/libArmDTPlatform.a',
        'ArmCpu': 'build/armcpu/libArmCpu.a',
        'SoftwareAES': 'build/software-aes/libSoftwareAES.a',
        'MiroUfs': 'build/miro-ufs/libMiroUfs.a',
        'VirtioMmioBlock': 'build/virtio-mmio-block/libVirtioMmioBlock.a',
    }
    for identifier, archive in expected.items():
        spec = specs.get(identifier, {})
        outputs = [path for path in spec.get('outputs', []) if path.endswith('.a')]
        check(archive in outputs, f'{identifier} catalog output is {archive} (got {outputs})')
        build = spec.get('build', [])
        check(build[:2] == ['build-arm-driver.py', identifier],
              f'{identifier} is built by build-arm-driver.py {identifier} (got {build})')
        if script:
            check(archive in script or archive in overrides, f'{identifier} links {archive}')


def find_catalog(device, tree):
    candidates = ([tree] if tree else []) + [device.parent / 'UniXNU', device, Path.cwd()]
    return next((base / 'components.toml' for base in candidates if (base / 'components.toml').is_file()), None)


def make_fixture(root, builder, catalog_path):
    """A build tree just big enough for build-arm-driver.py to emit a command line.

    The compiler and xcrun are stubs, so this runs on any host: the check is about which macros and output
    paths the builder emits, not whether the driver compiles (that needs the iOS SDK). The stub still writes
    a real ARM64 object header with an iOS 17 LC_BUILD_VERSION, because the builder audits its own output.
    """
    (root / 'scripts').mkdir(parents=True)
    shutil.copy2(builder, root / 'scripts/build-arm-driver.py')
    # The builder resolves component paths, source filenames and output directories from the catalog.
    shutil.copy2(catalog_path, root / 'components.toml')
    specs = {entry['id']: entry for entry in tomllib.loads(catalog_path.read_text())['component']}
    bin_dir = root / 'stub-bin'
    bin_dir.mkdir()

    clang = bin_dir / 'clang'
    # A compiler that behaves like the one CI resolves: upstream clang, which has no
    # -Wunnecessary-virtual-specifier (an Apple-only option). Like the real one it reports an unknown
    # -Wno- option as an error when -Werror is in effect, and stays quiet when the command also says
    # -Wno-unknown-warning-option.
    clang.write_text('#!/usr/bin/env python3\n'
                     'import struct, sys\n'
                     'arguments = sys.argv[1:]\n'
                     'known = {"-Wno-unused-parameter", "-Wno-c99-designator", "-Wno-reorder-init-list",\n'
                     '         "-Wno-deprecated-enum-enum-conversion", "-Wno-unknown-warning-option"}\n'
                     'unknown = [a for a in arguments if a.startswith("-Wno-") and a not in known]\n'
                     'if unknown and "-Werror" in arguments and "-Wno-unknown-warning-option" not in arguments:\n'
                     '    print(f"error: unknown warning option \'{unknown[0]}\' [-Werror,-Wunknown-warning-option]",\n'
                     '          file=sys.stderr)\n'
                     '    raise SystemExit(1)\n'
                     'out = sys.argv[sys.argv.index("-o") + 1]\n'
                     'header = struct.pack("<8I", 0xfeedfacf, 0x100000c, 0, 1, 1, 24, 0, 0)\n'
                     'build = struct.pack("<6I", 0x32, 24, 2, 0x110000, 0, 0)\n'
                     'open(out, "wb").write(header + build)\n')
    clang.chmod(0o755)

    xcrun = bin_dir / 'xcrun'
    xcrun.write_text('#!/usr/bin/env python3\n'
                     'import sys\n'
                     'arguments = sys.argv[1:]\n'
                     'if arguments[:2] == ["ar", "rcs"]:\n'
                     '    open(arguments[2], "wb").close()\n')
    xcrun.chmod(0o755)

    record = root / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    record.parent.mkdir(parents=True)
    source = root / 'iokit/IOPlatformExpert.cpp'
    source.parent.mkdir(parents=True)
    source.write_text('int reference;\n')
    json.dump({'directory': str(record.parent), 'file': str(source),
               'arguments': [str(clang), '-c', str(source), '-o', 'IOPlatformExpert.o', '-I', str(source.parent)]},
              record.open('w'))
    for identifier in ('MiroUfs', 'SoftwareAES', 'VirtioMmioBlock', 'ArmCpu', 'ArmDTPlatform'):
        spec = specs.get(identifier, {})
        name = next((item for item in spec.get('source_files', []) if item.endswith('.cpp')), identifier + '.cpp')
        (root / 'drivers' / identifier).mkdir(parents=True)
        (root / 'drivers' / identifier / name).write_text(f'int {identifier};\n')
    for directory in ('kits/VerifiedRoot/include', 'build/storage-abi/include', '.cache/xnu-work/osfmk'):
        (root / directory).mkdir(parents=True, exist_ok=True)


def run_builder(root, *arguments):
    environment = dict(os.environ, PATH=f'{root / "stub-bin"}{os.pathsep}{os.environ.get("PATH", "")}')
    return subprocess.run([sys.executable, str(root / 'scripts/build-arm-driver.py'), *arguments],
                          cwd=root, capture_output=True, text=True, env=environment)


def emitted_defines(root, directory):
    record = root / 'build' / directory / 'compile-command.json'
    if not record.is_file():
        return set()
    return set(re.findall(r'-DNATIVE_VERIFIED_BLOCK[\w]*(?:=\d+)?', record.read_text()))


def emitted_command(root, directory):
    record = root / 'build' / directory / 'compile-command.json'
    return record.read_text() if record.is_file() else ''


def check_verified_mode(builder, catalog_path):
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        make_fixture(root, builder, catalog_path)

        result = run_builder(root, 'MiroUfs')
        check(result.returncode == 0, f'build-arm-driver.py MiroUfs runs (exit {result.returncode})')
        defines = emitted_defines(root, 'miro-ufs')
        check(not defines, f'MiroUfs without --verified-root defines no verified block macro (got {sorted(defines)})')
        check((root / 'build/miro-ufs').is_dir(),
              'MiroUfs keeps its own output directory (build/miro-ufs)')
        check((root / 'build/miro-ufs/libMiroUfs.a').is_file(),
              'MiroUfs builds build/miro-ufs/libMiroUfs.a, the archive the kernel script links')
        # MiroUfs includes <IOKit/storage/IOBlockStorageDevice.h>, which the xnu headers do not carry: the
        # build tree stages the public IOStorageFamily headers in build/storage-abi/include (the base
        # scripts/unixnu/kernel.py does it before the archive builders) and the command line must point there.
        storage = str(root / 'build/storage-abi/include')
        command = json.loads(emitted_command(root, 'miro-ufs'))
        check(any(a == '-I' and b == storage for a, b in zip(command, command[1:])),
              'MiroUfs compiles with the staged IOStorageFamily include directory')

        result = run_builder(root, 'MiroUfs', '--verified-root')
        defines = emitted_defines(root, 'miro-ufs')
        check(result.returncode == 0 and '-DNATIVE_VERIFIED_BLOCK=1' in defines,
              f'MiroUfs --verified-root defines the verified block macro (got {sorted(defines)})')

        result = run_builder(root, 'MiroUfs', '--verified-root', '--verified-root-bundle')
        defines = emitted_defines(root, 'miro-ufs')
        check(result.returncode == 0 and '-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1' in defines,
              f'MiroUfs --verified-root-bundle defines the bundle macro (got {sorted(defines)})')

        # The virtio transport owns the verified qualification directories.
        result = run_builder(root, 'VirtioMmioBlock', '--verified-root')
        check(result.returncode == 0 and (root / 'build/virtio-mmio-block-verified').is_dir(),
              'VirtioMmioBlock --verified-root writes build/virtio-mmio-block-verified')

        # Guards that existed before this port and must not have been relaxed.
        result = run_builder(root, 'SoftwareAES', '--verified-root')
        check(result.returncode != 0 and 'read-only block transport' in result.stderr,
              'a non-storage component still refuses --verified-root')
        result = run_builder(root, 'MiroUfs', '--verified-root-bundle')
        check(result.returncode != 0 and 'requires verified block mode' in result.stderr,
              'a bundle still requires --verified-root')


def conditional_depth(path):
    """Return, for each line, how many #if blocks it is inside."""
    depths = []
    depth = 0
    for line in path.read_text().splitlines():
        depths.append(depth)
        stripped = line.strip()
        if stripped.startswith('#if'):
            depth += 1
        elif stripped.startswith('#endif'):
            depth -= 1
    return depths


def guarded_definitions(lines, depths):
    """Names declared at file scope inside a conditional guard, with the line that declares them.

    Only unindented declarations count. A call inside a guarded block is a use, not a definition, and
    leaving it in the set would hide the very defect this looks for: a call site at the guard's own
    depth would make the declaration look guarded all the way through.
    """
    declared = {}
    for index, line in enumerate(lines):
        if not depths[index] or line[:1].isspace() or line.lstrip().startswith(('//', '*', '#')):
            continue
        match = re.match(r'\s*(?:volatile\s+|static\s+|inline\s+|extern\s+|const\s+|unsigned\s+|struct\s+|class\s+|'
                         r'[A-Za-z_]\w*[\s*&]+)+([A-Za-z_]\w*)\s*(?:=|\()', line)
        if match:
            declared.setdefault(match.group(1), index)
            # "volatile uint64_t a = 0, b = 0;" declares b on the same line, and a symbol that is declared
            # only here is exactly what this check is about.
            for extra in re.findall(r',\s*([A-Za-z_]\w*)\s*(?==|,|;)', line):
                declared.setdefault(extra, index)
    return declared


def check_verified_symbols(device):
    """A symbol declared only under NATIVE_VERIFIED_BLOCK must not be used outside that guard.

    The plain NativeUfs build -- the one board.toml [kernel] archives lists, without --verified-root --
    does not define NATIVE_VERIFIED_BLOCK, so an unguarded use of timerTicks() or native_ufs_read_ticks
    fails with "use of undeclared identifier". The CI compile reported exactly that, three errors in
    total, while the declaration sat twenty lines above inside the guard.

    This is a preprocessing question, not a compiler question, so it needs no Xcode.
    """
    for source in sorted((device / 'drivers').glob('*/*.cpp')):
        lines = source.read_text().splitlines()
        depths = conditional_depth(source)
        relative = source.relative_to(device)
        for name, declared_at in sorted(guarded_definitions(lines, depths).items()):
            if len(name) < 4:
                continue
            uses = [index + 1 for index, line in enumerate(lines)
                    if index != declared_at and depths[index] == 0
                    and not line.lstrip().startswith(('//', '*'))
                    and re.search(rf'\b{re.escape(name)}\b', line)]
            check(not uses,
                  f'{relative}: {name} is declared only under NATIVE_VERIFIED_BLOCK (line {declared_at + 1}) '
                  f'but used outside it: lines {uses}')
        check_unverified_visibility(source, lines, relative)


# Static helpers the plain build must still see. Each is used by the plain path (publishThread's
# readiness test, the root layout probe) and is not part of the verified policy machinery. A guard placed
# one line too high makes them vanish from the plain build, and the compiler then reports "use of
# undeclared identifier" a thousand lines below the mistake -- which is what happened when an
# `#if NATIVE_VERIFIED_BLOCK` meant to wrap mediaWithUuidExists was closed after publishThread instead.
# check_verified_symbols only looks for the opposite fault (declared guarded, used unguarded), so it
# passed while CI failed to compile.
def verified_only_depth(lines):
    """For each line, whether it is reachable only when NATIVE_VERIFIED_BLOCK is defined.

    `conditional_depth` counts every #if, including `#ifdef NATIVE_UFS_BLOCK_DEVICE`, which is true in
    the plain build too, so it cannot answer "would the plain build see this line". This tracks the two
    macros this port actually switches on: a line is verified-only when the innermost enclosing
    condition mentioning NATIVE_VERIFIED_BLOCK is a positive test. An `#else` inverts it.
    """
    state = []
    result = []
    for line in lines:
        result.append(any(state))
        stripped = line.strip()
        if not stripped.startswith('#'):
            continue
        word = stripped[1:].strip()
        if word.startswith(('if', 'ifdef', 'ifndef')):
            condition = word[2:].strip() if word.startswith('ifdef') or word.startswith('ifndef') else word
            negated = word.startswith('ifndef') or re.search(r'!\s*defined\s*\(?\s*NATIVE_VERIFIED_BLOCK', condition)
            mentions = 'NATIVE_VERIFIED_BLOCK' in condition
            state.append(bool(mentions and not negated))
        elif word.startswith('else') and state:
            mentions = 'NATIVE_VERIFIED_BLOCK' in word or state[-1] is not None
            state[-1] = not state[-1] if mentions else state[-1]
        elif word.startswith('endif') and state:
            state.pop()
    return result


PLAIN_BUILD_SYMBOLS = ('publishThread', 'discardPendingWindow', 'captureMissingMedia',
                       'mediaUnderWindow', 'ufsMediaHasAncestor')
# mediaWithUuidExists is deliberately verified-only: it matches on UUIDs that only a signed policy
# provides. The others are the plain publication path and must exist in both builds.
VERIFIED_ONLY_SYMBOLS = ('mediaWithUuidExists',)


def check_unverified_visibility(source, lines, relative):
    """Symbols the plain NativeUfs build needs must not sit inside a verified-only block."""
    verified_only = verified_only_depth(lines)
    for index, line in enumerate(lines):
        if line[:1].isspace() or line.lstrip().startswith(('//', '*', '#')):
            continue
        for name in PLAIN_BUILD_SYMBOLS:
            if re.search(rf'\b{name}\s*\(', line):
                check(not verified_only[index],
                      f'{relative}: {name} is defined at line {index + 1} inside a block only the verified '
                      f'build compiles, so the plain build would not see it')
        for name in VERIFIED_ONLY_SYMBOLS:
            if re.search(rf'\b{name}\s*\(', line):
                check(verified_only[index],
                      f'{relative}: {name} is defined at line {index + 1} outside the verified-only block; '
                      f'the plain build would then carry a UUID matcher it has no policy for')


def check_binding_slide(device, tree=None):
    """The storage bindings must take the prelink slide, and must only move the kext's own addresses.

    build-storage-kext-bindings.py reads the original kernelcache, but the transport is linked into the
    kernel that emit_prelink.py has already rebased: it moves every component segment to "original va +
    slide". A `.set` of a kext-owned slot is an absolute address, so it has to move by that slide too, or
    every call lands 0x1000000 below the real function. Kernel exports must not move: prelink resolves
    those against the newly built kernel, so this image only supplies the name.

    These are source properties (no kernelcache and no Xcode needed), which is why they can be checked here.
    """
    candidates = ([tree] if tree else []) + [device, device.parent / 'UniXNU']
    script = next((base / 'scripts/build-storage-kext-bindings.py' for base in candidates
                   if (base / 'scripts/build-storage-kext-bindings.py').is_file()), None)
    if script is None:
        # A checkout that only has the base repository: the board builder is not next to it.
        return
    text = script.read_text()
    check("'--slide'" in text or '--slide' in text,
          'build-storage-kext-bindings.py accepts the prelink slide')
    check("UNIXNU_PRELINK_SLIDE" in text,
          'build-storage-kext-bindings.py falls back to UNIXNU_PRELINK_SLIDE from the kernel step')
    check('inside_kext' in text,
          'build-storage-kext-bindings.py decides by "inside the kext", not by export-table membership')
    # The assembly is written from `bound` only; if the kernel-export slots leaked into it they would be
    # pinned to the original image's layout.
    check(".set {name}, {address:#x}" in text and 'for name, address in sorted(bound.items())' in text,
          'build-storage-kext-bindings.py writes .set only for kext-owned slots')
    kernel = device.parent / 'UniXNU/scripts/unixnu/kernel.py'
    if kernel.is_file():
        check('UNIXNU_PRELINK_SLIDE' in kernel.read_text(),
              'the kernel step hands the board prelink slide to the bindings builder')
    check_reference_is_bytes(text)


def check_reference_is_bytes(text):
    """The reference object must reach the Mach-O parser as bytes, not as a Path.

    native_prelink.macho() indexes into the image (`at + 32 > len(data)`), so a Path raises
    "object of type 'PosixPath' has no len()" only once the kernel and every other archive have already
    been built -- which is exactly how CI failed, at archive 10/10. The kernelcache is read with
    read_bytes() a few lines below; the reference object was not. Assert the call site passes bytes.
    """
    # Look at the call sites only: the definition line is "def reference_vtable(reference):", and a
    # call is any other line that names the function.
    calls = [line.strip() for line in text.splitlines()
             if 'reference_vtable(' in line and not line.strip().startswith('def ')]
    check(bool(calls), 'build-storage-kext-bindings.py calls reference_vtable')
    check(all('read_bytes()' in call for call in calls),
          f'build-storage-kext-bindings.py passes the reference object as bytes: {calls}')
    check('reference.is_file()' in text,
          'build-storage-kext-bindings.py checks the reference object exists before reading it')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--device', type=Path, help='device repository (default: the one this script is in)')
    parser.add_argument('--tree', type=Path, help='assembled build tree, to check the kernel script too')
    arguments = parser.parse_args()

    device = arguments.device or DEVICE
    # Inside the assembled tree this script lives in scripts/tests, but board.toml only exists in the device
    # repository: reach it through the tree's recorded device, or through the directory it was copied from.
    if not (device / 'board.toml').is_file():
        for base in device.parents[:3]:
            state = base / 'state.json'
            recorded = json.loads(state.read_text()).get('device') if state.is_file() else None
            if recorded and (Path(recorded) / 'board.toml').is_file():
                device = Path(recorded)
                break
    builder = device / 'scripts/build-arm-driver.py'
    if not check(builder.is_file(), f'{builder.name} is in the build tree'):
        report()
        return 1

    # The firmware preflight runs from the assembled tree, so the kernel script is right there. Without it
    # the archive-path checks are skipped and only the config and builder checks run.
    tree = arguments.tree
    if tree is None:
        candidate = Path.cwd()
        if (candidate / 'scripts/build-xnu-4k-baseline.sh').is_file():
            tree = candidate

    check_archives(device, tree)
    check_products(device, tree)
    catalog = find_catalog(device, tree)
    if check(catalog is not None, 'components.toml is available for the builder checks'):
        check_verified_mode(builder, catalog)
    check_verified_symbols(device)
    check_binding_slide(device, tree)
    return report()


def report():
    for message in passes:
        print(f'PASS {message}')
    for message in failures:
        print(f'FAIL {message}')
    print(f'{len(passes)} passed, {len(failures)} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
