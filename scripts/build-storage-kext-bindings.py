#!/usr/bin/env python3
"""Bind a storage transport's IOBlockStorageDevice imports to the kernelcache kext.

A block transport derives from IOBlockStorageDevice, which is not part of the xnu tree: it is a class
of Apple's separate IOStorageFamily project, and the target kernelcache already carries
com.apple.iokit.IOStorageFamily. There are only two ways to give the transport a base class:

  - compile IOBlockStorageDevice.cpp and link it, which registers a second class of that name and
    makes OSMetaClass panic ("class IOBlockStorageDevice is a duplicate; kext __kernel__ already has
    a class by that name", libkern/c++/OSMetaClass.cpp);
  - resolve the transport's imports to the addresses the prelinked kext has at run time.

scripts/build-native-virtio-bindings.py takes the second road for the virtio transports, but it
reads build/storage-abi/interface-audit.json, which scripts/audit-storage-interface.py produces from
build/darwinboot-storage-native-metadata/gdb-stop.json and a prelink candidate
(build/native-prelink/native-apfs-load-positive.macho). Neither input has a builder in this tree:
they are made by hand in a maintainer's HVF session, and the audit JSON is not committed. That is
why the virtio path cannot be reproduced in CI.

This script takes the same road without the live observation. The audit's method list and original
targets are recoverable from images the build already has:

  - the base class dispatch shape is the reference object of build-storage-classes.py, which
    compiles the same pinned IOBlockStorageDevice.cpp the transport compiles its header from;
  - the original vtable is the one run of 190 pointers in the kext's __DATA_CONST whose entries are
    either an original kernel export or an address inside the kext's own __TEXT_EXEC. That
    signature is what makes the identification unique: the audit records 176 kernel bindings and
    14 pure virtual slots, and the inherited-dispatch tables nearby have 33+ kext-internal
    entries, so they cannot be confused with it;
  - the metaclass, the protected constructor and the base destructor are decoded from that
    vtable's anchors with the same AArch64 instruction patterns the audit uses.

The bundle's symbol table is empty in the kernelcache (the nlist is stripped), so names cannot be
used for the original class. They are not needed: the vtable carries every target the transport can
import, and the reference vtable gives the slot each target belongs to, which is what turns an
import name into an address.
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
import native_prelink as np

KEXT = 'com.apple.iokit.IOStorageFamily'
BASE = 'IOBlockStorageDevice'
# The iOS 17 class shape the audit qualifies: 190 virtual slots, 14 of them pure in the base.
SLOTS = 190
PURE = 14
# The audit's expected split of the original vtable: 176 slots are original kernel exports, the
# other 14 are the kext's own pure-virtual traps.
KERNEL_BINDINGS = 176


def require(value, message):
    if not value:
        raise SystemExit(message)


def macho(data, at=0):
    return np.macho(data, at)


def symbols(data, image):
    """(name, value, kind) for every entry of an image's symbol table."""
    tables = [(offset, length) for command, offset, length in image['commands'] if command == 2]
    require(len(tables) == 1 and tables[0][1] == 24, 'Expected exactly one 24-byte symbol table')
    symoff, count, stroff, strsize = struct.unpack_from('<4I', data, tables[0][0] + 8)
    require(symoff + count * 16 <= len(data) and stroff + strsize <= len(data), 'Symbol table outside image')
    strings = data[stroff:stroff + strsize]
    result = []
    for index in range(count):
        strx, kind, _, _, value = struct.unpack_from('<IBBHQ', data, symoff + index * 16)
        if strx >= strsize:
            continue
        end = strings.find(b'\0', strx)
        if end < 0:
            continue
        result.append((strings[strx:end].decode(errors='replace'), value, kind))
    return result


def kext_bundle(cache):
    """Locate the IOStorageFamily bundle: (top-level segments, header offset, bundle image)."""
    parent = macho(cache)['segments']
    info = next(s for s in parent if s['name'] == '__PRELINK_INFO')
    at = np.backed_offset(parent, info['va'], info['filesize'], len(cache))
    metadata = np.unserialize_xml(cache[at:at + info['filesize']])
    entries = {e['CFBundleIdentifier']: e for e in metadata['_PrelinkInfoDictionary']}
    require(KEXT in entries, f'{KEXT} is not in the kernelcache prelink dictionary')
    entry = entries[KEXT]
    address = entry.get('_PrelinkExecutableLoadAddr')
    require(address and entry.get('_PrelinkExecutableSize'),
            f'{KEXT} carries no executable in this kernelcache')
    header = np.backed_offset(parent, address, 32, len(cache))
    bundle = macho(cache, header)
    require(bundle['kind'] == 11, f'{KEXT} is not an MH_KEXT_BUNDLE')
    return parent, header, bundle


def reference_vtable(reference):
    """The 190 method names of the reference base class, in slot order. `reference` is the object's bytes."""
    image = macho(reference)
    require(image['kind'] == 1, 'Expected an ordinary ARM64 reference object')
    entries = symbols(reference, image)
    vtable = next((value for name, value, kind in entries if name == '__ZTV' + str(len(BASE)) + BASE), None)
    require(vtable is not None, f'the reference object has no vtable for {BASE}')
    end = min(value for name, value, kind in entries
              if not kind & 0xe0 and kind & 0xe == 0xe and value > vtable)
    relocations = {}
    for segment in image['segments']:
        for section in segment['sections']:
            if section['va'] <= vtable < section['va'] + section['size']:
                ro, rc = struct.unpack_from('<II', reference, section['command_offset'] + 56)
                for index in range(rc):
                    address, word = struct.unpack_from('<II', reference, ro + index * 8)
                    if word >> 27 & 1 and word >> 28 == 0 and word >> 25 & 3 == 3:
                        relocations[section['va'] + address] = entries[word & 0xffffff][0]
    methods = []
    for at in range(vtable + 16, end, 8):
        if at not in relocations:
            break
        methods.append(relocations[at])
    require(len(methods) == SLOTS and methods.count('___cxa_pure_virtual') == PURE,
            f'the reference {BASE} dispatch shape changed ({len(methods)} slots, '
            f'{methods.count("___cxa_pure_virtual")} pure)')
    return methods


def original_vtable(cache, parent, image, methods):
    """Find the kext's own vtable in the kernelcache.

    The reference vtable says what each of the 190 slots must be, and that is what identifies the
    original one -- not a scan for "some table of 190 addresses":

      - a slot the reference marks pure-virtual must be a trap: either the kernel's own
        ___cxa_pure_virtual export (the prelink binds the kext's reference to it) or code inside
        the kext, which is where an unrelocated BRK trap lives;
      - a slot whose reference name is a kernel export must hold exactly that export;
      - any other slot is a method the kext owns, so it must point inside the kext's code.

    Two further properties make the match unique rather than merely plausible: the 16 bytes in
    front of the vtable are zero, and the base class is abstract, so the run that satisfies the
    pure-virtual slots is the class dispatch and not a neighbouring inherited table.

    Every read goes through the top-level segments. A bundle's own fileoffs still describe the
    original kernelcache layout, so mapping through the bundle would read the wrong bytes.
    """
    const = next((s for s in image['segments'] if s['name'] == '__DATA_CONST'), None)
    require(const is not None, f'{KEXT} has no __DATA_CONST segment to scan')
    text = next(s for s in image['segments'] if s['name'] == '__TEXT_EXEC' and s['size'])
    exports = np.exports(cache, macho(cache))
    by_name = {name: address for address, names in exports.items() for name in names}
    pure = by_name.get('___cxa_pure_virtual')
    require(pure is not None, 'The kernelcache exports no ___cxa_pure_virtual')
    pure_slots = [index for index, name in enumerate(methods) if name == '___cxa_pure_virtual']
    require(len(pure_slots) == PURE, f'the reference class has {len(pure_slots)} pure-virtual slots, not {PURE}')
    at = np.backed_offset(parent, const['va'], const['size'], len(cache))
    blob = cache[at:at + const['size']]

    def owned(pointer):
        return text['va'] <= pointer < text['va'] + text['size']

    def agrees(pointers):
        for index, name in enumerate(methods):
            pointer = pointers[index]
            if index in pure_slots:
                if pointer != pure and not owned(pointer):
                    return False
            elif name in by_name:
                if pointer != by_name[name]:
                    return False
            elif not owned(pointer):
                return False
        return True

    found = []
    for offset in range(16, len(blob) - SLOTS * 8 + 1, 8):
        if blob[offset - 16:offset] != bytes(16):
            continue
        pointers = struct.unpack_from('<' + 'Q' * SLOTS, blob, offset)
        if agrees(pointers):
            found.append((const['va'] + offset - 16, pointers))
    require(len(found) == 1,
            f'Cannot uniquely identify the original {BASE} dispatch ({len(found)} candidates)')
    return found[0]


def decode_metaclass(cache, parent, image, pointers):
    """Recover the original metaclass address by decoding the class's metaclass getter.

    The getter is ADRP+ADD of the metaclass followed by RET. Slots 0 and 1 are the abstract
    complete- and deleting-destructor traps, and slot 0's trap is four bytes ahead of slot 1's, so
    slot 7 is the getter: the return-type and alloc slots between them are the ones the metaclass
    itself owns. Only the address is needed here, never the metaclass contents: those live in the
    kext's __common and are filled at run time, which is why the audit needs a live observation and
    this does not.
    """
    getter = pointers[7]
    at = np.backed_offset(parent, getter, 12, len(cache))
    a, add, ret = struct.unpack_from('<3I', cache, at)
    require(a & 0x9f00001f == 0x90000000 and add & 0xffc003ff == 0x91000000 and ret == 0xd65f03c0,
            'Unqualified original storage metaclass getter')
    immediate = ((a >> 5 & 0x7ffff) << 2) | (a >> 29 & 3)
    if immediate & 0x100000:
        immediate -= 0x200000
    metaclass = ((getter & ~4095) + (immediate << 12)) + (add >> 10 & 4095)
    data = next(s for s in image['segments'] if s['name'] == '__DATA' and s['size'])
    section = next((s for s in data['sections']
                    if (s.get('name') or s.get('sectname')) == '__common'), None)
    require(section is not None, f'{KEXT} has no __common section for its metaclass')
    require(section['va'] <= metaclass < section['va'] + section['size'] and metaclass % 8 == 0,
            f'the decoded metaclass {metaclass:#x} is not in the kext metaclass storage')
    return metaclass


def decode_destructor(cache, parent, image, pointers):
    """The base destructor is the branch immediately before the first slot (the abstract D0 trap)."""
    at = np.backed_offset(parent, pointers[0] - 4, 4, len(cache))
    word = struct.unpack_from('<I', cache, at)[0]
    require(word & 0xfc000000 == 0x14000000, 'The original base destructor is not a direct branch')
    immediate = word & 0x3ffffff
    if immediate & 0x2000000:
        immediate -= 0x4000000
    return (pointers[0] - 4) + immediate * 4


def decode_constructor(cache, parent, image, table):
    """The constructor stores the vtable into a new object, so it materialises table + 16."""
    text = next(s for s in image['segments'] if s['name'] == '__TEXT_EXEC' and s['size'])
    found = []
    for address in range(text['va'], text['va'] + text['size'] - 32, 4):
        at = np.backed_offset(parent, address, 32, len(cache))
        words = struct.unpack_from('<8I', cache, at)
        if (words[:2] != (0xa9bf7bfd, 0x910003fd) or words[2] & 0xfc000000 != 0x94000000 or
                words[3] & 0x9f00001f != 0x90000008 or words[4] & 0xffc003ff != 0x91000108 or
                words[5:] != (0xf9000008, 0xa8c17bfd, 0xd65f03c0)):
            continue
        immediate = ((words[3] >> 5 & 0x7ffff) << 2) | (words[3] >> 29 & 3)
        if immediate & 0x100000:
            immediate -= 0x200000
        stored = ((address + 12) & ~4095) + immediate * 4096 + (words[4] >> 10 & 4095)
        if stored == table + 16:
            found.append(address)
    require(len(found) == 1, f'Cannot uniquely identify the original {BASE} constructor '
                             f'({len(found)} candidates)')
    return found[0]


def imports(object_path, base_mangled):
    """Undefined external symbols of the compiled transport object, keyed by what they need."""
    data = object_path.read_bytes()
    return [name for name, value, kind in symbols(data, macho(data))
            if not kind & 0xe0 and not kind & 0x0e and base_mangled in name]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--kernelcache', type=Path,
                        help='the original kernelcache the board is qualified against '
                             '(default: resolve it from board.toml [ipsw], as the prelink step does)')
    parser.add_argument('--component', default='MiroUfs',
                        help='canonical transport component ID (or its legacy name)')
    parser.add_argument('--class', dest='class_name', help='transport runtime class name (default: <runtime name>Block)')
    parser.add_argument('--directory', type=Path,
                        help='the transport build directory (default: the catalog output directory)')
    parser.add_argument('--object', type=Path,
                        help='the compiled transport object (default: <directory>/<source>.o)')
    parser.add_argument('--reference', type=Path,
                        help='the compiled IOBlockStorageDevice reference object '
                             '(default: build/storage-classes/IOBlockStorageDevice.o)')
    parser.add_argument('--record', type=Path, help='optional JSON record of what was bound')
    parser.add_argument('--slide', type=lambda value: int(value, 0),
                        help='the rebase the prelink step applies, as board.toml [kernel.prelink] slide. '
                             'Addresses inside the kext move by it; kernel exports do not '
                             '(default: 0, the un-rebased kernelcache)')
    args = parser.parse_args()

    # The catalog gives the canonical output directory, archive and source file, so a legacy name in an
    # older board file still resolves to the same artifact the kernel link reads.
    spec = {}
    catalog = ROOT / 'components.toml'
    if catalog.is_file():
        spec = next((entry for entry in tomllib.loads(catalog.read_text()).get('component', [])
                     if args.component in (entry.get('id'), entry.get('legacy_name'))), {})
    canonical = spec.get('id', args.component)
    archive_output = next((path for path in spec.get('outputs', []) if path.endswith('.a')), None)
    source = next((name for name in spec.get('source_files', []) if name.endswith('.cpp')), None)
    runtime = spec.get('legacy_name', canonical)
    class_name = args.class_name or spec.get('runtime_class') or (runtime + 'Block')
    default_directory = (Path(archive_output).parent if archive_output
                         else Path('build') / ('miro-ufs' if canonical in ('MiroUfs', 'ShengUfs')
                                              else 'native-' + canonical.lower()))
    directory = (args.directory or ROOT / default_directory).resolve()
    object_path = (args.object or directory / ((Path(source).stem if source else runtime) + '.o')).resolve()
    archive_name = Path(archive_output).name if archive_output else 'lib' + canonical + '.a'
    reference = args.reference or ROOT / 'build/storage-classes/IOBlockStorageDevice.o'
    cache_path = args.kernelcache
    if cache_path is None:
        # The kernel step resolves the kernelcache before the archives run (the link needs the bindings, the
        # bindings need the image) and hands it over as UNIXNU_KERNELCACHE, which is the same option name the
        # prelink step reads. Nothing is downloaded twice: that step caches the decompressed image.
        value = os.environ.get('UNIXNU_KERNELCACHE')
        require(value, 'no kernelcache: pass --kernelcache or set UNIXNU_KERNELCACHE '
                       '(the kernel step sets it when this builder is listed in board.toml archives)')
        cache_path = Path(value)
    require(cache_path.is_file(), f'{cache_path} missing')

    require(reference.is_file(), f'{reference} missing')

    # Both parsers below index into the image, so they take its bytes, not its path.
    methods = reference_vtable(reference.read_bytes())
    cache = cache_path.read_bytes()
    parent, _, bundle = kext_bundle(cache)
    table, pointers = original_vtable(cache, parent, bundle, methods)
    metaclass = decode_metaclass(cache, parent, bundle, pointers)
    destructor = decode_destructor(cache, parent, bundle, pointers)
    constructor = decode_constructor(cache, parent, bundle, table)

    # The image read above is the original kernelcache, but the transport is linked into the rebased
    # kernel: emit_prelink.py moves every component segment to "original va + slide" (it rebases the
    # segment and section headers, and the pointer sites it rewrites). A slot that points inside the
    # kext therefore has to move by the same slide, or the transport would call a stale address
    # 0x1000000 below the real one.
    #
    # A slot that is a kernel export (the pure-virtual trap above all) must not be written at all:
    # the linker resolves it against the newly built kernel, and a .set of this image's address would
    # both override that resolution and hardcode a layout the new kernel does not have.
    #
    # "Inside the kext" is the only sound test. The image's export table holds both kinds (the
    # kext's own symbols are exports too, which is what makes it empty-looking: nsyms=0 for the
    # bundle, so the names come from the top level), so membership in it says nothing.
    slide = args.slide if args.slide is not None else int(os.environ.get('UNIXNU_PRELINK_SLIDE', '0'), 0)
    owned = [(s['va'], s['va'] + s['size']) for s in bundle['segments'] if s['size'] and s['name'] != '__LINKEDIT']

    def inside_kext(address):
        return any(start <= address < end for start, end in owned)

    def rebase(address):
        return address + slide if inside_kext(address) else address

    # The reference vtable names every slot; the original vtable holds the address for each. An
    # import is bound to the address of the slot whose reference name it is, so the transport calls
    # the kext's own implementation and nothing of the base class is compiled in.
    original = {name: pointers[index] for index, name in enumerate(methods)}
    original['__ZTV' + str(len(BASE)) + BASE] = table
    original['__ZN' + str(len(BASE)) + BASE + '10gMetaClassE'] = metaclass
    original['__ZN' + str(len(BASE)) + BASE + 'C2EPK11OSMetaClass'] = constructor
    original['__ZN' + str(len(BASE)) + BASE + 'D2Ev'] = destructor
    by_name = {name: rebase (address) for name, address in original.items()}

    require(object_path.is_file(), f'{object_path} missing; build the transport archive first')
    wanted = imports(object_path, str(len(BASE)) + BASE)
    missing = [name for name in wanted if name not in by_name]
    if missing:
        for name in missing:
            print(f'no address for {name}', file=sys.stderr)
        raise SystemExit(f'{len(missing)} storage import(s) have no original slot')

    # Only addresses the kext owns become .set constants. An import whose original slot is a kernel
    # export keeps the name unresolved, so the link binds it to the newly built kernel instead of to
    # this image's layout (the pure-virtual trap is the one that matters; the 176 export slots are
    # methods the kext's vtable also names, and any of them can be called through the base).
    bound = {name: by_name[name] for name in wanted if inside_kext (original[name])}
    delegated = sorted(name for name in wanted if not inside_kext (original[name]))
    assembly = directory / 'original-bindings.S'
    assembly.write_text(''.join(f'.globl {name}\n.set {name}, {address:#x}\n'
                                for name, address in sorted(bound.items())))
    result = subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Werror',
                             '-Wno-incompatible-sysroot', '-c', assembly, '-o',
                             directory / 'original-bindings.o'], capture_output=True, text=True)
    if result.returncode:
        print(result.stdout + result.stderr, flush=True)
        raise SystemExit('the bindings assembly did not assemble; the output above is the whole log')

    archive = directory / archive_name
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', archive, object_path, directory / 'original-bindings.o'],
                   check=True)

    record = dict(kernelcache_sha256=hashlib.sha256(cache).hexdigest(), kext=KEXT, source='kernelcache',
                  kext_symbols_stripped=True, reference_vtable_slots=SLOTS,
                  original_dispatch=hex(table), original_metaclass=hex(metaclass),
                  original_constructor=hex(constructor), original_destructor=hex(destructor),
                  kernel_bindings=KERNEL_BINDINGS, pure_virtual_slots=PURE,
                  prelink_slide=hex(slide),
                  rebased_slots=sum(1 for name in bound if by_name[name] != original[name]),
                  concrete_class=class_name, bound={name: hex(address) for name, address in sorted(bound.items())},
                  delegated_to_linker=delegated,
                  compiled_object_sha256=hashlib.sha256(object_path.read_bytes()).hexdigest(),
                  actual_io_qualified=False)
    (directory / 'bindings.json').write_text(json.dumps(record, indent=2) + '\n')
    if args.record:
        args.record.write_text(json.dumps(record, indent=2) + '\n')
    print(f'PASS: {SLOTS} original slots; {KERNEL_BINDINGS} kernel bindings, {PURE} pure; '
          f'{len(bound)} {BASE} imports bound to {KEXT}, {len(delegated)} to the linked kernel '
          f'(prelink slide {slide:#x})')
    print('Actual device I/O and filesystem integration remain unqualified by this compile/ABI step.')


if __name__ == '__main__':
    main()
