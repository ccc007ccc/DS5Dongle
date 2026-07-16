#!/usr/bin/env python3
"""Verify the exact 512-to-480 interpolation-period reduction.

The firmware's generic resampler represents each fractional position with a
480 denominator.  For 512 input frames and 480 output frames the remainder is
always ``i * 32`` for ``i`` in 0..14, so the rounded interpolation can use an
equivalent denominator of 15.
"""


def c_div(numerator: int, denominator: int) -> int:
    """Integer division with C99 truncation toward zero."""
    if numerator >= 0:
        return numerator // denominator
    return -((-numerator) // denominator)


def generic_delta(delta: int, fraction: int) -> int:
    scaled = delta * fraction
    scaled += 240 if scaled >= 0 else -240
    return c_div(scaled, 480)


def period15_delta(delta: int, index: int) -> int:
    scaled = delta * index
    scaled += 7 if scaled >= 0 else -7
    return c_div(scaled, 15)


def main() -> int:
    source_frame = 0
    fraction = 0
    for output_frame in range(480):
        block, index = divmod(output_frame, 15)
        expected_source = block * 16 + index
        expected_fraction = index * 32
        assert source_frame == expected_source
        assert fraction == expected_fraction

        fraction += 512
        if fraction >= 960:
            source_frame += 2
            fraction -= 960
        else:
            source_frame += 1
            fraction -= 480

    for delta in range(-65535, 65536):
        for index in range(15):
            assert generic_delta(delta, index * 32) == period15_delta(delta, index)

    print("m61 512->480 period-15 resampler equivalence: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
