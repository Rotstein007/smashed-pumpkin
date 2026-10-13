import importlib.util
from pathlib import Path
import tempfile
import unittest
import re
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("release_notes", ROOT / "build-aux/release-notes.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ReleaseNotesTest(unittest.TestCase):
    def test_project_notes_match_the_about_dialog_and_version(self):
        meson = (ROOT / "meson.build").read_text(encoding="utf-8")
        version = re.search(r"version:\s*'([^']+)'", meson).group(1)
        notes = MODULE.release_notes(MODULE.METAINFO, version)
        source = (ROOT / "src/app.c").read_text(encoding="utf-8")
        about = source.split("pumpkin_app_about_action(", 1)[1]
        about = about.split("adw_about_dialog_set_developer_name", 1)[0]
        notes_version = re.search(
            r'adw_about_dialog_set_release_notes_version\(about, "([^"]+)"\)', about)
        self.assertEqual(notes_version.group(1), version)
        # English source notes may be wrapped in gettext and preceded by
        # translator comments; still compare the unchanged bilingual literals.
        markups = [match.group("markup") for match in re.finditer(
            r'adw_about_dialog_set_release_notes\(\s*about,\s*'
            r'(?:/\*.*?\*/\s*)*'
            r'(?P<translated>_\(\s*)?(?P<markup>(?:"[^"]*"\s*)+)'
            r'(?(translated)\))\s*\);', about, re.DOTALL)]
        self.assertEqual(len(markups), 2)
        for heading, markup in zip(("Deutsch", "English"), markups):
            section = notes.split(f"## {heading}\n\n", 1)[1].split("\n\n", 1)[0]
            actual = [line.split(". ", 1)[1] for line in section.splitlines()]
            xml = "".join(re.findall(r'"([^"]*)"', markup))
            expected = ["".join(item.itertext()) for item in ET.fromstring(xml)]
            self.assertEqual(actual, expected)

    def notes(self, description, version="1.2.3", extra_release=""):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "metainfo.xml"
            path.write_text(
                f'<component><releases><release version="1.2.3">'
                f"<description>{description}</description></release>"
                f"{extra_release}</releases></component>", encoding="utf-8")
            return MODULE.release_notes(path, version)

    def test_languages_are_separated_and_notes_preserved(self):
        notes = self.notes(
            '<ul><li>First change.</li><li xml:lang="de">Erste Aenderung.</li>'
            '<li>Second change.</li><li xml:lang="de">Zweite Aenderung.</li></ul>')
        self.assertEqual(notes,
                         "## English\n\n1. First change.\n2. Second change.\n\n"
                         "## Deutsch\n\n1. Erste Aenderung.\n2. Zweite Aenderung.\n")

    def test_explicit_english_and_other_translations(self):
        notes = self.notes('<ul><li xml:lang="en">Change.</li>'
                           '<li xml:lang="fr">Changement.</li>'
                           '<li xml:lang="de">Aenderung.</li></ul>')
        self.assertNotIn("Changement", notes)
        self.assertIn("1. Change.", notes)
        self.assertIn("1. Aenderung.", notes)

    def test_incomplete_translation_is_rejected(self):
        for description in ("", "<ul/>", "<ul><li>Change.</li></ul>",
                            '<ul><li>Change.</li><li xml:lang="de"> </li></ul>'):
            with self.subTest(description=description), self.assertRaises(ValueError):
                self.notes(description)

    def test_unsupported_markup_is_not_silently_discarded(self):
        for description in ("<p>Change.</p>", '<ul xml:lang="de"><li>Change.</li></ul>',
                            "<ul><p>Change.</p></ul>"):
            with self.subTest(description=description), self.assertRaises(ValueError):
                self.notes(description)

    def test_missing_duplicate_or_invalid_version_is_rejected(self):
        for version in ("2.0.0", "v1.2.3", "1.2", "1.2.3\n"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                self.notes("<ul/>", version)
        with self.assertRaises(ValueError):
            self.notes("<ul/>", extra_release='<release version="1.2.3"/>')


if __name__ == "__main__":
    unittest.main()
