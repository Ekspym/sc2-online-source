"""Default.xbe from a Soulcalibur II (USA) Xbox disc image, as the port takes it: the image's XDVDFS read (the same way as
the launcher's disc.cpp), and the retail executable given extract-xiso's media patch (one byte, docs/bring-up.md).
    python -B tools/xiso_xbe.py <image.iso> <out/Default.xbe>"""
import hashlib, struct, sys

SECTOR = 2048
PARTITIONS = (0, 0x18300000, 0xFD90000, 0x89D80000)
PATCHED = '7ce12851850ab46766f77c612b251afe7c8bd81f879e11b4836bdf766be542c6'
RETAIL = 'd7c2e1fc993350821e3f7e13cfd1c9d4ba6a7bf91011005bdd1f843899efb730'
PATCH_AT, PATCH_RETAIL, PATCH_BYTE = 0xF68C7, 0x7D, 0xEB


def root(f):
    for p in PARTITIONS:
        f.seek(p + 32 * SECTOR)
        d = f.read(28)
        if d.startswith(b'MICROSOFT*XBOX*MEDIA'):
            sector, size = struct.unpack_from('<II', d, 20)
            f.seek(p + sector * SECTOR)
            return p, f.read(size)
    sys.exit('not an Xbox disc image')


def find(f, partition, directory, name):
    want, at = name.upper(), 0
    for _ in range(4096):
        left, right, sector, size, attr, length = struct.unpack_from('<HHIIBB', directory, at)
        if left == 0xFFFF and right == 0xFFFF:
            break
        entry = directory[at + 14:at + 14 + length].decode('latin-1').upper()
        if entry == want:
            f.seek(partition + sector * SECTOR)
            return f.read(size)
        nxt = left if want < entry else right
        if not nxt:
            break
        at = nxt * 4
    sys.exit('%s not on the disc' % name)


def main(image, out):
    with open(image, 'rb') as f:
        partition, directory = root(f)
        xbe = bytearray(find(f, partition, directory, 'default.xbe'))
    digest = hashlib.sha256(xbe).hexdigest()
    if digest == RETAIL:
        if xbe[PATCH_AT] != PATCH_RETAIL:
            sys.exit('unexpected byte at the media check')
        xbe[PATCH_AT] = PATCH_BYTE
        digest = hashlib.sha256(xbe).hexdigest()
        print('retail executable, media patch applied')
    if digest != PATCHED:
        sys.exit('not Soulcalibur II (USA): %s' % digest[:12])
    open(out, 'wb').write(xbe)
    print('Default.xbe %d bytes, %s' % (len(xbe), digest[:12]))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
