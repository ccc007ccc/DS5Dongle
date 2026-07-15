#!/usr/bin/env python3

USB_FRAMES = 512
OUTPUT_FRAMES = 480


def main() -> None:
    src_frame = 0
    frac = 0
    for output_frame in range(OUTPUT_FRAMES):
        numerator = output_frame * USB_FRAMES
        assert src_frame == numerator // OUTPUT_FRAMES
        assert frac == numerator % OUTPUT_FRAMES
        assert src_frame + 1 < USB_FRAMES

        frac += USB_FRAMES
        if frac >= 2 * OUTPUT_FRAMES:
            src_frame += 2
            frac -= 2 * OUTPUT_FRAMES
        else:
            src_frame += 1
            frac -= OUTPUT_FRAMES

    assert src_frame == USB_FRAMES
    assert frac == 0
    print("M61 512-to-480 resample phase test passed.")


if __name__ == "__main__":
    main()
