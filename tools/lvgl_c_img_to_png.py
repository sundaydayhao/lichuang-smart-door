#!/usr/bin/env python3
"""Recover PNG files from LVGL C image descriptors."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError as exc:  # pragma: no cover - friendly CLI error
    raise SystemExit("Pillow is required: python3 -m pip install Pillow") from exc


FORMATS = ("auto", "rgba8888", "rgb565-swap", "rgb565", "rgb332")


def parse_hex_bytes(text: str) -> list[int]:
    return [int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", text)]


def expand_rgb565(value: int) -> tuple[int, int, int]:
    r5 = (value >> 11) & 0x1F
    g6 = (value >> 5) & 0x3F
    b5 = value & 0x1F
    return (r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)


def expand_rgb332(value: int) -> tuple[int, int, int]:
    r3 = (value >> 5) & 0x07
    g3 = (value >> 2) & 0x07
    b2 = value & 0x03
    return round(r3 * 255 / 7), round(g3 * 255 / 7), round(b2 * 255 / 3)


def find_block(array_body: str, pattern: str) -> list[int] | None:
    match = re.search(pattern + r"(.*?)(?:#endif)", array_body, re.S)
    if not match:
        return None
    return parse_hex_bytes(match.group(1))


def find_format_data(array_body: str, image_format: str) -> tuple[str, list[int]]:
    patterns = {
        "rgba8888": r"#if\s+LV_COLOR_DEPTH\s*==\s*32",
        "rgb565-swap": r"#if\s+LV_COLOR_DEPTH\s*==\s*16\s*&&\s*LV_COLOR_16_SWAP\s*!=\s*0",
        "rgb565": r"#if\s+LV_COLOR_DEPTH\s*==\s*16\s*&&\s*LV_COLOR_16_SWAP\s*==\s*0",
        "rgb332": r"#if\s+LV_COLOR_DEPTH\s*==\s*1\s*\|\|\s*LV_COLOR_DEPTH\s*==\s*8",
    }

    choices = ("rgba8888", "rgb565-swap", "rgb565", "rgb332") if image_format == "auto" else (image_format,)
    for choice in choices:
        data = find_block(array_body, patterns[choice])
        if data is not None:
            return choice, data

    data = parse_hex_bytes(array_body)
    if data:
        return "raw", data

    raise ValueError("no image data bytes found")


def bytes_to_rgba(data: list[int], width: int, height: int, image_format: str) -> bytes:
    pixel_count = width * height
    rgba = bytearray()

    if image_format in ("rgba8888", "raw"):
        expected = pixel_count * 4
        if len(data) != expected:
            raise ValueError(f"expected {expected} bytes for {image_format}, got {len(data)}")
        return bytes(data)

    if image_format in ("rgb565", "rgb565-swap"):
        expected = pixel_count * 3
        if len(data) != expected:
            raise ValueError(f"expected {expected} bytes for {image_format}, got {len(data)}")
        for i in range(0, len(data), 3):
            b0, b1, alpha = data[i : i + 3]
            value = (b0 << 8) | b1 if image_format == "rgb565-swap" else b0 | (b1 << 8)
            rgba.extend((*expand_rgb565(value), alpha))
        return bytes(rgba)

    if image_format == "rgb332":
        expected = pixel_count * 2
        if len(data) != expected:
            raise ValueError(f"expected {expected} bytes for {image_format}, got {len(data)}")
        for i in range(0, len(data), 2):
            color, alpha = data[i : i + 2]
            rgba.extend((*expand_rgb332(color), alpha))
        return bytes(rgba)

    raise ValueError(f"unsupported image format: {image_format}")


def iter_image_descriptors(source: str):
    descriptor_pattern = re.compile(
        r"const\s+lv_img_dsc_t\s+(?P<name>\w+)\s*=\s*\{(?P<body>.*?)\n\};",
        re.S,
    )
    for descriptor in descriptor_pattern.finditer(source):
        body = descriptor.group("body")
        width = re.search(r"\.header\.w\s*=\s*(\d+)", body)
        height = re.search(r"\.header\.h\s*=\s*(\d+)", body)
        data = re.search(r"\.data\s*=\s*(\w+)", body)
        if not (width and height and data):
            continue
        yield descriptor.group("name"), int(width.group(1)), int(height.group(1)), data.group(1)


def extract_array(source: str, array_name: str) -> str:
    array_pattern = re.compile(
        r"uint8_t\s+"
        + re.escape(array_name)
        + r"\s*\[\]\s*=\s*\{(?P<body>.*?)\n\};",
        re.S,
    )
    match = array_pattern.search(source)
    if not match:
        raise ValueError(f"array not found: {array_name}")
    return match.group("body")


def convert_file(path: Path, output_dir: Path, image_format: str) -> list[tuple[Path, str, int, int]]:
    source = path.read_text(encoding="utf-8", errors="ignore")
    outputs: list[tuple[Path, str, int, int]] = []

    for name, width, height, array_name in iter_image_descriptors(source):
        array_body = extract_array(source, array_name)
        detected_format, data = find_format_data(array_body, image_format)
        rgba = bytes_to_rgba(data, width, height, detected_format)
        image = Image.frombytes("RGBA", (width, height), rgba)
        output_path = output_dir / f"{name}.png"
        image.save(output_path)
        outputs.append((output_path, detected_format, width, height))

    return outputs


def collect_inputs(inputs: list[Path]) -> list[Path]:
    files: list[Path] = []
    for path in inputs:
        if path.is_dir():
            files.extend(sorted(path.glob("*.c")))
        elif path.suffix == ".c":
            files.append(path)
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path, help="LVGL .c image file(s) or directories")
    parser.add_argument("-o", "--out-dir", type=Path, required=True, help="directory for recovered PNG files")
    parser.add_argument("--format", choices=FORMATS, default="auto", help="source array format to decode")
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    files = collect_inputs(args.inputs)
    if not files:
        print("No .c files found.", file=sys.stderr)
        return 1

    converted = 0
    for path in files:
        try:
            outputs = convert_file(path, args.out_dir, args.format)
        except ValueError as exc:
            print(f"skip {path}: {exc}", file=sys.stderr)
            continue
        for output_path, detected_format, width, height in outputs:
            print(f"{path} -> {output_path} ({width}x{height}, {detected_format})")
            converted += 1

    if converted == 0:
        print("No LVGL image descriptors were converted.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
