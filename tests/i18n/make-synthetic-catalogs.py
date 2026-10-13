#!/usr/bin/env python3
"""Generate synthetic MO catalogs for discovery edge cases.

Writes, under the fixture root given as argv[1]:
  xx_TRUNC/LC_MESSAGES/smashed-pumpkin.mo   a file shorter than the MO header
  xx_BADHDR/LC_MESSAGES/smashed-pumpkin.mo  a well-formed MO whose header is
                                            not valid UTF-8
  xx_BE/LC_MESSAGES/smashed-pumpkin.mo      a big-endian catalog with one
                                            translated entry ("Overview")

Discovery must skip the first two silently and accept the third. The
big-endian branch of the MO reader otherwise has zero coverage.
"""
import pathlib
import struct
import sys


def write_mo(path: pathlib.Path, entries: list[tuple[bytes, bytes]], big_endian: bool = False) -> None:
    fmt = ">I" if big_endian else "<I"
    count = len(entries)
    original_table = 28
    translation_table = original_table + 8 * count
    strings_start = translation_table + 8 * count

    original_blob = b"".join(msgid + b"\0" for msgid, _ in entries)
    translation_blob = b"".join(msgstr + b"\0" for _, msgstr in entries)

    original_offsets = []
    translation_offsets = []
    originals_pos = 0
    translations_pos = 0
    for msgid, msgstr in entries:
        # GNU MO offsets are absolute file offsets, matching msgfmt output.
        original_offsets.append(struct.pack(fmt + "I", len(msgid), strings_start + originals_pos))
        originals_pos += len(msgid) + 1
        translation_offsets.append(struct.pack(fmt + "I", len(msgstr),
                                               strings_start + len(original_blob) + translations_pos))
        translations_pos += len(msgstr) + 1

    header = struct.pack(fmt + "IIIIII", 0x950412DE, 0, count,
                         original_table, translation_table, 0, 0)
    path.write_bytes(header + b"".join(original_offsets) + b"".join(translation_offsets) +
                     original_blob + translation_blob)


def main() -> None:
    root = pathlib.Path(sys.argv[1])

    truncated = root / "xx_TRUNC" / "LC_MESSAGES"
    truncated.mkdir(parents=True, exist_ok=True)
    (truncated / "smashed-pumpkin.mo").write_bytes(b"\xde\x12\x04\x95" + b"\x00" * 20)

    bad_header = root / "xx_BADHDR" / "LC_MESSAGES"
    bad_header.mkdir(parents=True, exist_ok=True)
    write_mo(bad_header / "smashed-pumpkin.mo",
             [(b"", b"Content-Type: text/plain; charset=UTF-8\nX-Language-Name: \xff\xfe invalid\n")])

    big_endian = root / "xx_BE" / "LC_MESSAGES"
    big_endian.mkdir(parents=True, exist_ok=True)
    write_mo(big_endian / "smashed-pumpkin.mo",
             [(b"", "Content-Type: text/plain; charset=UTF-8\nX-Language-Name: Big-endian fixture\n".encode()),
              ("Overview".encode(), "BE-Übersicht".encode())],
             big_endian=True)


if __name__ == "__main__":
    main()
