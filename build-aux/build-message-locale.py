#!/usr/bin/env python3
"""Build only glibc's tiny LC_MESSAGES category; no runtime localedef needed."""
import pathlib
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="pumpkin-message-locale-") as profile:
    output = pathlib.Path(profile) / "locale"
    subprocess.run([sys.argv[1], "--no-archive", "-i", "en_US", "-f", "UTF-8", str(output)], check=True)
    data = (output / "LC_MESSAGES" / "SYS_LC_MESSAGES").read_bytes()
    target = pathlib.Path(sys.argv[2])
    target.write_bytes(data)
    # Mirror the installed hierarchy for uninstalled integration tests.
    category = target.parent / "message-locales/smashed.UTF-8/LC_MESSAGES"
    category.mkdir(parents=True, exist_ok=True)
    (category / "SYS_LC_MESSAGES").write_bytes(data)
