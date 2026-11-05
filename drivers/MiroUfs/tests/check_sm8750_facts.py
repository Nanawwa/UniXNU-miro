#!/usr/bin/env python3
"""Check the SM8750 device-tree facts MiroUfs depends on, against the stock tree.

Offline and read-only: it parses the compiled device tree (and, when present, the mainline Linux
clock driver) and fails if the driver's hardcoded compatible strings or GCC offsets stop matching
what the board actually publishes. Nothing here runs on hardware.

    python3 tests/check_sm8750_facts.py --dtb path/to/miro.dtb
"""
import argparse
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# The compatible strings MiroUfs::probe accepts, in the order of the accepted[] table.
ACCEPTED = ['qcom,ufshc', 'qcom,sm8750-ufshc']

# SM8750 branch offsets from gcc-sm8750.c.
# The second element is the clk_branch name in gcc-sm8750.c, spelled out instead of derived from the
# macro: the Linux names carry an underscore inside the symbol index (tx_symbol_0_clk).
CLOCKS = {
    'GCC_UFS_PHY_AXI_ENABLE': (0x77018, 'gcc_ufs_phy_axi_clk'),
    'GCC_UFS_PHY_AHB_ENABLE': (0x77028, 'gcc_ufs_phy_ahb_clk'),
    'GCC_UFS_PHY_TX_SYMBOL0_ENABLE': (0x7702c, 'gcc_ufs_phy_tx_symbol_0_clk'),
    'GCC_UFS_PHY_RX_SYMBOL0_ENABLE': (0x77030, 'gcc_ufs_phy_rx_symbol_0_clk'),
    'GCC_UFS_PHY_UNIPRO_CORE_ENABLE': (0x7706c, 'gcc_ufs_phy_unipro_core_clk'),
    'GCC_UFS_PHY_ICE_CORE_ENABLE': (0x7707c, 'gcc_ufs_phy_ice_core_clk'),
    'GCC_UFS_PHY_PHY_AUX_ENABLE': (0x770bc, 'gcc_ufs_phy_phy_aux_clk'),
    'GCC_UFS_PHY_RX_SYMBOL1_ENABLE': (0x770d8, 'gcc_ufs_phy_rx_symbol_1_clk'),
    'GCC_AGGRE_UFS_PHY_AXI_ENABLE': (0x770f0, 'gcc_aggre_ufs_phy_axi_clk'),
}
GDSCS = {'GCC_UFS_PHY_GDSCR': 0x77004, 'GCC_UFS_MEM_PHY_GDSCR': 0x9e000}
GCC_WINDOW = (0x77000, 0x1000)


def fail(message):
    print('FAIL: ' + message)
    return 1


def parse_fdt(data):
    """Yield (path, {prop: bytes}) for every node of a flattened device tree."""
    magic, totalsize, off_struct, off_strings = struct.unpack_from('>4I', data, 0)
    if magic != 0xD00DFEED:
        raise SystemExit('not a flattened device tree')
    version, = struct.unpack_from('>I', data, 20)
    if version < 16:
        raise SystemExit(f'unsupported FDT version {version}')
    size_struct, = struct.unpack_from('>I', data, 36)

    def string(offset):
        start = off_strings + offset
        end = data.index(b'\0', start)
        return data[start:end].decode()

    path = []
    end_struct = off_struct + size_struct
    at = off_struct
    while at < end_struct:
        token, = struct.unpack_from('>I', data, at)
        at += 4
        if token == 1:                                   # FDT_BEGIN_NODE
            end = data.index(b'\0', at)
            name = data[at:end].decode()
            at = (end + 4) & ~3
            path.append(name)
            yield '/'.join(path), None
        elif token == 2:                                 # FDT_END_NODE
            path.pop()
        elif token == 3:                                 # FDT_PROP
            length, nameoff = struct.unpack_from('>2I', data, at)
            at += 8
            yield '/'.join(path), (string(nameoff), data[at:at + length])
            at = (at + length + 3) & ~3
        elif token == 4:                                 # FDT_NOP
            continue
        elif token == 9:                                 # FDT_END
            break
        else:
            raise SystemExit(f'bad FDT token {token}')


def collect_facts(data):
    """{(node, prop): bytes} plus the node paths, for the trees checked below."""
    props, paths = {}, []
    for node, item in parse_fdt(data):
        if item is None:
            paths.append(node)
            continue
        name, value = item
        props[(node, name)] = value
    return props, paths


def ufshc_nodes(paths):
    return [p for p in paths if re.search(r'(^|/)ufs(hc)?@[0-9a-f]+$', p)]


def check_compatible(props, paths):
    nodes = ufshc_nodes(paths)
    if not nodes:
        return fail('the device tree has no ufs/ufshc node')
    matched = False
    for node in nodes:
        raw = props.get((node, 'compatible'))
        if raw is None:
            print(f'  {node}: no compatible property')
            continue
        strings = [s.decode() for s in raw.split(b'\0') if s]
        print(f'  {node}: {", ".join(strings)}')
        if any(s in ACCEPTED for s in strings):
            matched = True
    if not matched:
        return fail('no ufs node publishes a compatible MiroUfs accepts')
    # The concrete regression this guards: MiroUfs matches substrings of the published property, so
    # a tree that lists only a name outside ACCEPTED would silently not attach.
    print(f'PASS: a ufs node matches one of {ACCEPTED}')
    return 0


def check_offsets(source_header, clock_driver):
    text = source_header.read_text()
    missing, wrong = [], []
    for macro, (sm8750, _linux) in CLOCKS.items():
        found = re.search(rf'#define\s+{macro}\s+(0x[0-9a-fA-F]+)', text)
        if not found:
            missing.append(macro)
            continue
        if int(found.group(1), 16) != sm8750:
            wrong.append(f'{macro}={found.group(1)} (want {sm8750:#x})')
        low, high = GCC_WINDOW
        if not low <= sm8750 < low + high:
            wrong.append(f'{macro}={sm8750:#x} outside the mapped window')
    for macro, value in GDSCS.items():
        found = re.search(rf'#define\s+{macro}\s+(0x[0-9a-fA-F]+)', text)
        if not found:
            missing.append(macro)
        elif int(found.group(1), 16) != value:
            wrong.append(f'{macro}={found.group(1)} (want {value:#x})')
    if missing:
        return fail('GccUfs.h is missing ' + ', '.join(missing))
    if wrong:
        return fail('GccUfs.h offsets disagree with SM8750: ' + '; '.join(wrong))
    print(f'PASS: all {len(CLOCKS) + len(GDSCS)} GCC offsets are the SM8750 values and inside the mapped window')

    if clock_driver is None:
        print('  (no gcc-sm8750.c given; offsets are checked against the recorded table only)')
        return 0
    if not clock_driver.is_file():
        return fail(f'--clock-driver: no such file: {clock_driver}')
    driver = clock_driver.read_text()
    verified, unverified = 0, []
    for macro, (sm8750, name) in CLOCKS.items():
        # halt_reg is the branch's enable/halt register in the Linux clock driver.
        block = re.search(rf'static struct clk_branch {name} = \{{(.*?)\n\}};', driver, re.S)
        if not block:
            unverified.append(name)
            continue
        halt = re.search(r'\.halt_reg\s*=\s*(0x[0-9a-fA-F]+)', block.group(1))
        if not halt:
            unverified.append(name)
            continue
        if int(halt.group(1), 16) != sm8750:
            return fail(f'{name}: gcc-sm8750.c has halt_reg {halt.group(1)}, GccUfs.h has {sm8750:#x}')
        verified += 1
    if unverified:
        print(f'  not found in {clock_driver.name}: ' + ', '.join(unverified))
    print(f'PASS: {verified} offsets agree with halt_reg in {clock_driver.name}')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dtb', type=Path, required=True, help='compiled device tree of the board')
    parser.add_argument('--header', type=Path, default=ROOT / 'GccUfs.h')
    parser.add_argument('--clock-driver', type=Path, default=None,
                        help='mainline gcc-sm8750.c, to cross-check the offsets')
    args = parser.parse_args()
    for path in (args.dtb, args.header):
        if not path.is_file():
            raise SystemExit(f'missing file: {path}')
    props, paths = collect_facts(args.dtb.read_bytes())
    print(f'{args.dtb}: {len(paths)} nodes')
    status = check_compatible(props, paths)
    status |= check_offsets(args.header, args.clock_driver)
    return status


if __name__ == '__main__':
    sys.exit(main())
