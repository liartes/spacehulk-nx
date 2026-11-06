#!/usr/bin/env python3
"""fix_summary.py -- give a freestanding profile the summary of a normal run.

    python3 -I tools/pgo/fix_summary.py <pgo-profile folder>

libgcov writes each .gcda's object summary at exit: the number of runs and
sum_max, the largest arc counter of the whole program (libgcov-driver.c
compute_summary). A program that serializes its counters itself
(__gcov_info_to_gcda, source/sh_pgo.c) never exits through libgcov, so its
summaries say runs=0, sum_max=0 -- and GCC derives the hot-code threshold
from sum_max (predict.cc get_hot_bb_threshold). This rewrites every file's
summary as one run with the program-wide sum_max. Run it after each
merge-stream (merge_profile.sh does). MIT.
"""
import os
import struct
import sys

TAG_FUNCTION = 0x01000000
TAG_ARCS = 0x01A10000           # GCOV_TAG_FOR_COUNTER(GCOV_COUNTER_ARCS)
TAG_OBJECT_SUMMARY = 0xA1000000


def records(data):
    """(offset of the record's payload, tag, payload length) after the header."""
    if data[:4] != b"adcg":     # 'gcda' as a little-endian word
        raise ValueError("not a .gcda file")
    pos = 16                    # magic, version, stamp, checksum
    while pos + 8 <= len(data):
        tag, length = struct.unpack_from("<Ii", data, pos)
        pos += 8
        if tag == 0:
            break
        # counters that are all zero: a negative length, no payload
        length = max(length, 0)
        yield pos, tag, length
        pos += length


def max_arc(data):
    best = 0
    for pos, tag, length in records(data):
        if tag == TAG_ARCS:
            for i in range(length // 8):
                lo, hi = struct.unpack_from("<II", data, pos + 8 * i)
                best = max(best, lo | hi << 32)
    return best


def main():
    folder = sys.argv[1]
    files = sorted(os.path.join(folder, f) for f in os.listdir(folder) if f.endswith(".gcda"))
    blobs = {f: bytearray(open(f, "rb").read()) for f in files}
    sum_max = max(max_arc(b) for b in blobs.values())
    sum_max32 = min(sum_max, 0xFFFFFFFF)  # the record holds 32 bits
    fixed = 0
    for f, b in blobs.items():
        for pos, tag, length in records(b):
            if tag == TAG_OBJECT_SUMMARY and length == 8:
                struct.pack_into("<II", b, pos, 1, sum_max32)
                fixed += 1
                break
        open(f, "wb").write(b)
    print(f"{fixed} of {len(files)} summaries set to runs=1, sum_max={sum_max32} (largest arc count {sum_max})")


if __name__ == "__main__":
    main()
