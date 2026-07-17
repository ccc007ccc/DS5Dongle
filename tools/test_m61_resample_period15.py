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


def generic_sample(source: list[tuple[int, int]], output_frame: int, channel: int) -> int:
    position = output_frame * 512
    source_frame, fraction = divmod(position, 480)
    next_frame = min(source_frame + 1, 511)
    source_channel = channel ^ 1
    a = source[source_frame][source_channel]
    b = source[next_frame][source_channel]
    return a + generic_delta(b - a, fraction)


def period15_sample(source: list[tuple[int, int]], output_frame: int, channel: int) -> int:
    block, index = divmod(output_frame, 15)
    source_frame = block * 16 + index
    source_channel = channel ^ 1
    a = source[source_frame][source_channel]
    b = source[source_frame + 1][source_channel]
    return a + period15_delta(b - a, index)


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

    # The DualSense speaker transport's L/R order is opposite the Windows USB
    # channel order. Verify that the optimized path both preserves the exact
    # interpolation and swaps only the two speaker channels.
    source = [
        (((frame * 211 + 17) % 60001) - 30000,
         ((frame * 307 + 101) % 62003) - 31000)
        for frame in range(512)
    ]
    for output_frame in range(480):
        for channel in range(2):
            assert period15_sample(source, output_frame, channel) == generic_sample(
                source, output_frame, channel
            )

    print("m61 512->480 period-15 stereo-swap equivalence: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
