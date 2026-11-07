"""Resolve an ISO9660 logical sector to its containing file (read-only)."""
import struct
import sys

image, *queries = sys.argv[1:]
queries = [int(q, 0) for q in queries]
with open(image, "rb") as disc:
    def read_at(offset, size):
        disc.seek(offset)
        return disc.read(size)

    pvd = read_at(16 * 2048, 2048)
    if pvd[1:6] != b"CD001":
        raise SystemExit("Not an ISO9660 primary volume")
    seen = set()

    def walk(record, path):
        sector, size = struct.unpack_from("<I", record, 2)[0], struct.unpack_from("<I", record, 10)[0]
        if not record[25] & 2:
            for query in queries:
                if sector <= query < sector + (size + 2047) // 2048:
                    print(f"LBN {query}: {path} start={sector} size={size} offset={(query-sector)*2048}")
            return
        if sector in seen:
            return
        seen.add(sector)
        data = read_at(sector * 2048, size)
        pos = 0
        while pos < len(data):
            length = data[pos]
            if not length:
                pos = ((pos // 2048) + 1) * 2048
                continue
            child = data[pos:pos+length]
            name = child[33:33+child[32]]
            if name not in (b"\x00", b"\x01"):
                walk(child, path + "/" + name.decode("ascii", errors="replace"))
            pos += length

    walk(pvd[156:156+pvd[156]], "")
