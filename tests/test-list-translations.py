#!/usr/bin/env python3
"""Test configure-time PO discovery without gettext or Meson dependencies."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "build-aux/list-translations.py"
SPEC = importlib.util.spec_from_file_location("list_translations", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ListTranslationsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="pumpkin-translations-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.po = self.root / "po"
        self.po.mkdir()

    def add_catalog(self, name):
        (self.po / name).write_text('msgid "English"\nmsgstr "Deutsch"\n', encoding="utf-8")

    def test_added_zh_tw_needs_no_linguas_change(self):
        linguas = self.po / "LINGUAS"
        linguas.write_text("", encoding="utf-8")
        self.add_catalog("de.po")
        self.assertEqual(MODULE.list_translations(self.po), ["de"])
        self.add_catalog("zh_TW.po")
        self.assertEqual(MODULE.list_translations(self.po), ["de", "zh_TW"])
        self.assertEqual(linguas.read_text(encoding="utf-8"), "")

    def test_removed_catalog_disappears_on_the_next_scan(self):
        self.add_catalog("de.po")
        self.add_catalog("zh_TW.po")
        self.assertEqual(MODULE.list_translations(self.po), ["de", "zh_TW"])
        (self.po / "zh_TW.po").unlink()
        self.assertEqual(MODULE.list_translations(self.po), ["de"])

    def test_no_linguas_file_is_required(self):
        self.add_catalog("de.po")
        self.assertEqual(MODULE.list_translations(self.po), ["de"])
        self.assertFalse((self.po / "LINGUAS").exists())

    def test_linguas_contents_are_not_used(self):
        (self.po / "LINGUAS").write_text("fr\n../outside\n", encoding="utf-8")
        self.add_catalog("de.po")
        self.assertEqual(MODULE.list_translations(self.po), ["de"])

    def test_catalogs_are_sorted_by_filename_stem(self):
        for name in ("zh_TW.po", "sr@latin.po", "pt_BR.po", "de.po", "zh_Hant_TW.po"):
            self.add_catalog(name)
        self.assertEqual(MODULE.list_translations(self.po),
                         ["de", "pt_BR", "sr@latin", "zh_Hant_TW", "zh_TW"])

    def test_no_catalogs_returns_an_empty_list(self):
        (self.po / "POTFILES").write_text("src/app.c\n", encoding="utf-8")
        (self.po / "messages.pot").write_text("", encoding="utf-8")
        self.assertEqual(MODULE.list_translations(self.po), [])

    def test_invalid_filenames_are_rejected(self):
        for name in (".po", "de DE.po", "-de.po", "DE.po", "de.po.po",
                     "de;touch.po", "de\\escape.po", "de\n.po", "de@@latin.po"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                path = Path(directory)
                (path / name).write_text("", encoding="utf-8")
                with self.assertRaises(ValueError):
                    MODULE.list_translations(path)

    def test_unsafe_stems_are_rejected(self):
        for name in ("../de", "/de", "de/../../outside", "de\\outside", "de@@fr",
                     "de\x00", "de\n", "de\r", "de\t", "de.UTF-8", "de@@"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                MODULE.validate_language(name)

    def test_symlink_cannot_escape_translation_directory(self):
        outside = self.root / "outside.po"
        outside.write_text('msgid "English"\nmsgstr "Deutsch"\n', encoding="utf-8")
        (self.po / "de.po").symlink_to(outside)
        with self.assertRaises(ValueError):
            MODULE.list_translations(self.po)

    def test_symlink_to_an_in_directory_catalog_is_also_rejected(self):
        self.add_catalog("de.po")
        (self.po / "fr.po").symlink_to("de.po")
        with self.assertRaises(ValueError):
            MODULE.list_translations(self.po)

    def test_directory_named_as_catalog_is_rejected(self):
        (self.po / "de.po").mkdir()
        with self.assertRaises(ValueError):
            MODULE.list_translations(self.po)

    def test_nested_catalogs_are_not_scanned(self):
        (self.po / "en").mkdir()
        (self.po / "en/de.po").write_text("", encoding="utf-8")
        self.add_catalog("zh_TW.po")
        self.assertEqual(MODULE.list_translations(self.po), ["zh_TW"])

    def test_optional_english_catalog_can_be_compiled(self):
        self.add_catalog("en.po")
        self.assertEqual(MODULE.list_translations(self.po), ["en"])

    def test_command_prints_one_sorted_language_per_line(self):
        self.add_catalog("zh_TW.po")
        self.add_catalog("de.po")
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.po)],
                                capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "de\nzh_TW\n")
        self.assertEqual(result.stderr, "")

    def test_command_empty_directory_prints_nothing(self):
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.po)],
                                capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "")

    def test_command_rejects_invalid_filename_without_partial_output(self):
        self.add_catalog("de.po")
        self.add_catalog("--invalid.po")
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.po)],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn("Invalid translation filename stem", result.stderr)

    def test_command_rejects_missing_directory(self):
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.root / "missing")],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertTrue(result.stderr)


if __name__ == "__main__":
    unittest.main()
