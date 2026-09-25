"""Independent filesystem oracle; no scanner code or packed-record decoder."""
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
    return sorted(items, key=lambda x: (int(x['device']), int(x['parent']), bytes.fromhex(x['name_hex'])))


def oracle(root: Path, task: str = 'tree', size_contract: str = 'logical') -> list[dict]:
    device = os.lstat(root).st_dev
    result = []

    def descend(path: bytes) -> int:
        parent = os.lstat(path).st_ino
        total = 0
        with os.scandir(path) as entries:
            children = sorted(entries, key=lambda entry: entry.name)
        for item in children:
            info = item.stat(follow_symlinks=False)
            if info.st_dev != device:
                continue
            code = kind(info.st_mode)
            size = 0
            if code == 1 and task != 'enumerate':
                size = allocated_size(item.path) if size_contract == 'allocated' else info.st_size
            row = {'device': str(info.st_dev), 'id': str(info.st_ino), 'parent': str(parent),
                   'name_hex': item.name.hex(), 'kind': code, 'valid': 31, 'size': size,
                   'subtree_bytes': 0, 'subtree_unknown': 0}
            result.append(row)
            if code == 2:
                subtree = descend(item.path)
                if task == 'tree':
                    row['subtree_bytes'] = subtree
                total += subtree
            else:
                total += size
        return total

    descend(os.fsencode(root))
    return canonical(result)
