#!/usr/bin/env python3
"""Read the miro UFS diagnostic log written by package/BootLog.c.

  read-miro-log.py DEVICE [--output DIR]

DEVICE is a block device or image. Without --oops it is the root partition, where the writer keeps a log in the last 32 MiB:
block 0 is the header (magic MIROLOG2), blocks 1..32 the kernel message buffer and blocks 33.. a ring of 16-block text slots.
The header is written last in each round, so only the slots it counts are trusted.

With --oops, DEVICE is instead the board's crash-log partition (by-name/oops on this phone), which is what the panic record and
the bring-up probe are written to now: the last eight blocks hold a MIROPNC1 record and the block below it a MIROPRB1 probe.
Every block is scanned, so the record is found whatever the partition's size.
"""
import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

BLOCK = 4096
MAGIC = b'MIROLOG2'
PANIC = b'MIROPNC1'
PROBE = b'MIROPRB1'
HEADER = struct.Struct('<8sIIQQ' + 'I' * 14 + '32s')
WINDOW_BLOCKS = 8192
PANIC_BLOCKS = 8


def read_window(path):
    size = path.stat().st_size
    blocks = size // BLOCK
    if blocks < WINDOW_BLOCKS:
        sys.exit(f'{path}: {blocks} blocks is smaller than the {WINDOW_BLOCKS}-block window')
    with path.open('rb') as stream:
        stream.seek((blocks - WINDOW_BLOCKS) * BLOCK)
        return stream.read(WINDOW_BLOCKS * BLOCK)


def parse_panic(window):
    blob = window[(WINDOW_BLOCKS - PANIC_BLOCKS) * BLOCK:]
    if blob[:8] != PANIC:
        return None
    version, length, crc, blocks = struct.unpack_from('<IIII', blob, 8)
    text = blob[32:32 + length]
    return {
        'version': version, 'length': length, 'crc': crc, 'blocks': blocks,
        'crc_ok': (zlib.crc32(text) & 0xFFFFFFFF) == crc,
        'text': text.decode(errors='replace'),
    }


def parse_probe(block):
    if block[:8] != PROBE:
        return None
    return {
        'status': struct.unpack_from('<I', block, 8)[0],
        'store_geometry': block[12], 'log_open': block[13], 'host_ready': block[14],
        'partition_last_lba': struct.unpack_from('<Q', block, 16)[0],
        'why': block[24:88].split(b'\0')[0].decode(errors='replace'),
    }


def oops_report(path):
    blob = path.read_bytes()
    blocks = len(blob) // BLOCK
    found = 0
    for index in range(blocks):
        block = blob[index * BLOCK:(index + 1) * BLOCK]
        if block[:8] == PANIC:
            version, length, crc, record_blocks = struct.unpack_from('<IIII', block, 8)
            text = block[32:32 + length]
            ok = (zlib.crc32(text) & 0xFFFFFFFF) == crc
            print(f"block {index}: panic record v{version} {length} bytes crc {'ok' if ok else 'BAD'} "
                  f"in {record_blocks} blocks")
            print('--- panic text ---')
            print(text.decode(errors='replace'))
            found += 1
        elif block[:8] == PROBE:
            probe = parse_probe(block)
            print(f"block {index}: probe status=0x{probe['status']:02x} why={probe['why']!r} "
                  f"store={probe['store_geometry']} log={probe['log_open']} host={probe['host_ready']} "
                  f"partition_last_lba={probe['partition_last_lba']}")
            found += 1
    if not found:
        sys.exit(f'{path}: no {PANIC!r} record and no {PROBE!r} probe in {blocks} blocks '
                 f'(the record is written by the panic screen; check that the boot reached it)')
    return 0


def parse(window):
    fields = HEADER.unpack_from(window, 0)
    (magic, version, header_bytes, boot_id, first_lba, window_blocks, msg_first, msg_blocks, msg_bytes,
     slot_first, slot_blocks, slot_count, slots_written, uptime_s, rounds, write_errors, gpt_open, gpt_last,
     state, tag) = fields
    if magic != MAGIC:
        return None
    base = 0
    message = window[msg_first * BLOCK: msg_first * BLOCK + msg_bytes]
    slots = []
    for index in range(min(slots_written, slot_count)):
        at = (slot_first + index * slot_blocks) * BLOCK
        slots.append(window[at: at + slot_blocks * BLOCK])
    return {
        'version': version, 'header_bytes': header_bytes, 'boot_id': boot_id, 'first_lba': first_lba,
        'window_blocks': window_blocks, 'slot_count': slot_count, 'slots_written': slots_written,
        'uptime_s': uptime_s, 'rounds': rounds, 'write_errors': write_errors,
        'gpt_open': gpt_open, 'gpt_last': gpt_last, 'state': state,
        'tag': tag.split(b'\0')[0].decode(errors='replace'),
        'message': message, 'slots': slots,
    }


def rows(blob):
    for at in range(0, len(blob) - 120, 121):
        line = blob[at:at + 120].split(b'\0')[0].decode(errors='replace').rstrip()
        if line:
            yield line


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('device', type=Path)
    parser.add_argument('--output', type=Path, help='directory for the decoded text and the raw window')
    parser.add_argument('--oops', action='store_true', help='DEVICE is the crash-log partition, not the root partition')
    args = parser.parse_args()
    if args.oops:
        sys.exit(oops_report(args.device))
    window = read_window(args.device)
    panic = parse_panic(window)
    if panic:
        print(f"panic record v{panic['version']} {panic['length']} bytes crc {'ok' if panic['crc_ok'] else 'BAD'} "
              f"({panic['blocks']} blocks)")
        print('--- panic text ---')
        print(panic['text'])
    log = parse(window)
    if not log:
        if panic:
            return
        sys.exit('no MIROLOG2 header and no panic record: neither channel wrote (check miro-log=1 and the boot-argument list)')
    print(f"boot_id {log['boot_id']} rounds {log['rounds']} uptime {log['uptime_s']}s state {log['state']} "
          f"tag {log['tag']!r} gpt {log['gpt_open']}->{log['gpt_last']} errors {log['write_errors']}")
    print(f"window {log['window_blocks']} blocks at LBA {log['first_lba']}, slots {log['slots_written']}/{log['slot_count']}")
    print('--- kernel message buffer ---')
    text = log['message'].decode(errors='replace').rstrip('\0')
    print(text[-20000:])
    print('--- newest board text ---')
    newest = rows(log['slots'][-1]) if log['slots'] else []
    for line in list(newest)[:60]:
        print(line)
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / 'window.bin').write_bytes(window)
        (args.output / 'header.json').write_text(json.dumps(
            {k: v for k, v in log.items() if k not in ('message', 'slots')}, indent=2) + '\n')
        (args.output / 'message.txt').write_text(text + '\n')
        (args.output / 'board.txt').write_text('\n'.join(newest) + '\n')
        print(f'wrote {args.output}')


if __name__ == '__main__':
    main()