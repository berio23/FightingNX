"""Lists the XDK libraries a default.xex links statically (XEX optional header 0x000200FF), with their versions.

Each game links its own copy of the XDK's Direct3D (D3D9 / XGRAPHC); its version decides the device layout that
Renut's renderer reads (docs/deko3d-renderer-guide.md, section 2).

    python tools/xex_static_libs.py assets/game_root/default.xex
"""
import struct
import sys

data = open(sys.argv[1], 'rb').read()
magic, flags, pe_offset, _, sec_offset, count = struct.unpack('>4sIIIII', data[:24])
if magic != b'XEX2':
    sys.exit(f'not an XEX2 file: {magic!r}')
for i in range(count):
    key, value = struct.unpack('>II', data[24 + 8 * i:32 + 8 * i])
    if key != 0x000200FF:
        continue
    size = struct.unpack('>I', data[value:value + 4])[0]
    for off in range(value + 4, value + size, 16):
        name, major, minor, build, qfe = struct.unpack('>8sHHHH', data[off:off + 16])
        approval = ['unapproved', 'possible', 'approved', 'expired'][(qfe >> 13) & 3]
        label = name.rstrip(b'\x00').decode()
        print(f'{label:10s} {major}.{minor}.{build}.{qfe & 0x1FFF}  (approval {approval})')
    break
else:
    sys.exit('no static library header (0x000200FF)')
