#!/usr/bin/env python3
"""List safe, sorted gettext catalog names without consulting LINGUAS."""

import argparse
from pathlib import Path
import re
import sys


# Accept language, optional territory/script components, and a locale modifier.
# Restrict names to ASCII tokens safe for Meson paths and gettext's --langs list.
LANGUAGE_NAME = re.compile(r"[a-z]{2,3}(?:_[A-Za-z0-9]+)*(?:@[A-Za-z0-9]+(?:[_-][A-Za-z0-9]+)*)?", re.ASCII)


def validate_language(name):
    """Reject path separators, whitespace, options, and malformed locale names."""
    if LANGUAGE_NAME.fullmatch(name) is None:
        raise ValueError(f"Invalid translation filename stem: {name!r}")
    return name


def list_translations(directory):
    """Scan only regular top-level PO files; English needs no source catalog."""
    directory = Path(directory)
    languages = []
    for path in directory.iterdir():
        if not path.name.endswith(".po"):
            continue
        language = validate_language(path.name[:-3])
        # Do not follow links to catalogs outside the source translation directory.
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"Translation catalog must be a regular file: {path.name!r}")
        languages.append(language)
    # An explicit en.po is still a valid optional gettext catalog.
    return sorted(languages)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="source directory containing PO files")
    args = parser.parse_args(argv)
    try:
        languages = list_translations(args.directory)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")
    for language in languages:
        print(language)
    return 0


if __name__ == "__main__":
    sys.exit(main())
