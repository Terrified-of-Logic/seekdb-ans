#!/usr/bin/env python3
"""Protocol double for the hipVS build helper, used without an AMD GPU.

It implements the same command line, start-byte handshake and native HNSW
record layout as build_helper.cpp, but it only copies the staged vectors into
fixed-size records with the original document ids in the label slot. The result
is deliberately not a searchable graph: seekdb must validate every record and
then take the explicit CPU fallback. This exercises the parent-side pre-start,
handshake, file exchange, linear validation, log capture and fallback paths.
"""
import struct
import sys

MAGIC = 0x315350564849


def main():
    argv = sys.argv[1:]
    wait_for_stdin = "--wait-stdin" in argv
    argv = [arg for arg in argv if arg != "--wait-stdin"]
    if len(argv) != 2:
        print("usage: test_fake_helper.py INPUT OUTPUT [--wait-stdin]", file=sys.stderr)
        return 2
    source, target = argv
    if wait_for_stdin:
        start = sys.stdin.buffer.read(1)
        if start != b"B":
            print("HIPVS_GPU_BUILD_FAILED fallback=false error=no start byte", file=sys.stderr)
            return 3
    with open(source, "rb") as handle:
        header = struct.unpack("<5Q", handle.read(40))
        rows, dim, m = header[1], header[2], header[3]
        if header[0] != MAGIC:
            raise SystemExit("bad magic")
        ids = struct.unpack("<%dq" % rows, handle.read(8 * rows))
        vectors = handle.read(4 * rows * dim)
        if len(vectors) != 4 * rows * dim:
            raise SystemExit("truncated vectors")
    row_size = 2 * m * 4 + 4 + dim * 4 + 8
    label_offset = 2 * m * 4 + 4 + dim * 4
    data_offset = 2 * m * 4 + 4
    file_header = struct.pack("<12Q", 0, 0, rows, row_size, label_offset, data_offset, 0, 0, 0, 0, 0, 0)
    with open(target, "wb") as output:
        output.write(file_header)
        for row in range(rows):
            record = bytearray(row_size)
            start = row * dim * 4
            record[data_offset:data_offset + dim * 4] = vectors[start:start + dim * 4]
            struct.pack_into("<q", record, label_offset, ids[row])
            output.write(record)
    print(
        "HIPVS_GPU_BUILD_SUCCESS device=fake rows=%d dim=%d seconds=0.001 "
        "init_ms=1 wait_ms=0 load_ms=1 cagra_ms=0 convert_ms=0 rewrite_ms=0 fallback=false"
        % (rows, dim),
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
