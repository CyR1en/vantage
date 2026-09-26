"""Independent filesystem oracle for Vantage's tree and hard-link accounting."""
from __future__ import annotations

import ctypes
import os
from pathlib import Path
import stat
import struct
import sys


def kind(mode: int) -> int:
    for predicate, code in ((stat.S_ISREG, 1), (stat.S_ISDIR, 2), (stat.S_ISBLK, 3),
                            (stat.S_ISCHR, 4), (stat.S_ISLNK, 5), (stat.S_ISSOCK, 6), (stat.S_ISFIFO, 7)):
        if predicate(mode):
            return code
    return 0


class AttrList(ctypes.Structure):
    _fields_ = [('bitmapcount', ctypes.c_uint16), ('reserved', ctypes.c_uint16),
                ('common', ctypes.c_uint32), ('volume', ctypes.c_uint32),
                ('directory', ctypes.c_uint32), ('file', ctypes.c_uint32),
                ('fork', ctypes.c_uint32)]


def allocated_size(path: bytes) -> int:
    """Ask only for ATTR_FILE_ALLOCSIZE; parse its fixed 4+8-byte reply.

    Darwin sys/attr.h defines bitmapcount=5, ALLOCSIZE=4, NOFOLLOW=1.
    With no RETURNED_ATTRS or variable fields, there is no shared bulk layout.
    """
    if sys.platform != 'darwin':
        raise RuntimeError('ATTR_FILE_ALLOCSIZE oracle requires macOS')
    libc = ctypes.CDLL(None, use_errno=True)
    call = libc.getattrlist
    call.argtypes = [ctypes.c_char_p, ctypes.POINTER(AttrList), ctypes.c_void_p,
                     ctypes.c_size_t, ctypes.c_ulong]
    call.restype = ctypes.c_int
    attrs = AttrList(bitmapcount=5, file=4)
    buffer = ctypes.create_string_buffer(12)
    if call(path, ctypes.byref(attrs), buffer, len(buffer), 1):
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), path)
    length, size = struct.unpack('=Iq', buffer.raw)
    if length != 12 or size < 0:
        raise AssertionError(f'invalid isolated allocation response: {buffer.raw.hex()}')
    return size


def canonical(items: list[dict]) -> list[dict]:
    return sorted(items, key=lambda row: bytes.fromhex(row['path_hex']))


def oracle(root: Path, size_contract: str = 'logical') -> dict:
    if size_contract not in ('logical', 'allocated'):
        raise ValueError(f'unknown size contract: {size_contract}')
    device = os.lstat(root).st_dev
    result = []
    unique = {}

    def descend(path: bytes, relative: bytes = b'') -> int:
        total = 0
        with os.scandir(path) as entries:
            children = sorted(entries, key=lambda entry: entry.name)
        for item in children:
            info = item.stat(follow_symlinks=False)
            if info.st_dev != device:
                continue
            code = kind(info.st_mode)
            size = 0
            if code == 1:
                size = allocated_size(item.path) if size_contract == 'allocated' else info.st_size
                unique[(info.st_dev, info.st_ino)] = size
            child = relative + (b'/' if relative else b'') + item.name
            row = {'path_hex': child.hex(), 'kind': code, 'bytes': size, 'unknown_sizes': 0}
            result.append(row)
            if code == 2:
                row['bytes'] = descend(item.path, child)
            total += row['bytes']
        return total

    total = descend(os.fsencode(root))
    return {'entries': canonical(result), 'total_bytes': total,
            'unique_bytes': sum(unique.values()), 'files': sum(row['kind'] == 1 for row in result),
            'directories': sum(row['kind'] == 2 for row in result), 'unknown_sizes': 0}
