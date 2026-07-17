#!/usr/bin/env python3
"""Check the 16-entry DualSense CRC32 update against the bitwise reference."""

import random

TABLE = (
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
    0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
)

def bitwise(crc, data):
    for value in data:
        crc ^= value
        for _ in range(8):
            mask = 0xFFFFFFFF if crc & 1 else 0
            crc = ((crc >> 1) ^ (0xEDB88320 & mask)) & 0xFFFFFFFF
    return crc

def nibble(crc, data):
    for value in data:
        crc ^= value
        crc = ((crc >> 4) ^ TABLE[crc & 15]) & 0xFFFFFFFF
        crc = ((crc >> 4) ^ TABLE[crc & 15]) & 0xFFFFFFFF
    return crc

def main():
    rng = random.Random(0xD5C032)
    for seed in (0, 1, 0xFFFFFFFF, 0x12345678):
        for value in range(256):
            data = bytes((value,))
            assert nibble(seed, data) == bitwise(seed, data)
    for length in (0, 1, 74, 138, 394, 543, 547):
        for _ in range(512):
            seed = rng.getrandbits(32)
            data = rng.randbytes(length)
            assert nibble(seed, data) == bitwise(seed, data)
    print("DualSense CRC32 nibble-table equivalence: PASS")

if __name__ == "__main__":
    main()
