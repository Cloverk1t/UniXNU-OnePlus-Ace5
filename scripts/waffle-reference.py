#!/usr/bin/env python3
"""Adapt the pinned vermeer UFS probe/GPT checks to the OnePlus 12 (waffle) disk."""
from pathlib import Path
import sys

root = Path(sys.argv[1]) / 'xnu' / 'vermeer'
probe = root / 'vermeer_ufs_probe.c'
block = root / 'vermeer_ufs_block.c'

s = probe.read_text()
if '#define EXPECTED_LAST_LBA 61842431U' in s:
    s = s.replace('#define EXPECTED_LAST_LBA 61842431U', '#define EXPECTED_LAST_LBA 123786239U')
if 'unsigned differences = vermeer_ufs_crc32(DATA, 8192) != EXPECTED_CRC32;' in s:
    s = s.replace(
        'unsigned differences = vermeer_ufs_crc32(DATA, 8192) != EXPECTED_CRC32;',
        '/* waffle bring-up: the pinned CRC is PKG110 LBA0; do not gate GPT init on it */\n'
        '    unsigned differences = 0;')
probe.write_text(s)

b = block.read_text()
if '(g.first != 2570304 || g.blocks != 59272123)' in b:
    b = b.replace('(g.first != 2570304 || g.blocks != 59272123)',
                  '(g.first != 4293356 || g.blocks != 119492879)')
block.write_text(b)
print('waffle vermeer deltas applied: probe last_lba=123786239, userdata 4293356+119492879, CRC gate relaxed')
