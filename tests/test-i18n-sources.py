#!/usr/bin/env python3
"""Check real gettext extraction inputs, template domains and data boundaries."""

import pathlib
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(sys.argv.pop(1)).resolve()
XGETTEXT = sys.argv.pop(1)
DOMAIN = "smashed-pumpkin"


class I18nSourcesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.paths = [
            line.strip()
            for line in (ROOT / "po/POTFILES").read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        ]
        with tempfile.TemporaryDirectory(prefix="smashed-pumpkin-pot-") as directory:
            output = pathlib.Path(directory) / "test.pot"
            subprocess.run(
                [XGETTEXT, "--from-code=UTF-8", "--add-comments=Translators:",
                 "--keyword=_", "--keyword=N_", "--keyword=C_:1c,2",
                 "--keyword=NC_:1c,2", "--keyword=g_dngettext:2,3",
                 "--keyword=ngettext:1,2", "--flag=g_strdup_printf:1:c-format",
                 f"--directory={ROOT}", f"--files-from={ROOT / 'po/POTFILES'}",
                 f"--output={output}"], check=True, cwd=ROOT,
            )
            cls.pot = output.read_text(encoding="utf-8")

    def test_templates_share_domain(self):
        for relative in ("resources/ui/window.ui", "resources/ui/window-windows.ui"):
            with self.subTest(template=relative):
                self.assertIn(relative, self.paths)
                root = ET.parse(ROOT / relative).getroot()
                self.assertEqual(root.attrib.get("domain"), DOMAIN)
                brand = root.find("./template/property[@name='title']")
                self.assertEqual(brand.text, "Smashed Pumpkin")
                self.assertNotEqual(brand.attrib.get("translatable"), "yes")
                self.assertIn(relative, self.pot)

    def test_language_selector_structure(self):
        for relative in ("resources/ui/window.ui", "resources/ui/window-windows.ui"):
            with self.subTest(template=relative):
                root = ET.parse(ROOT / relative).getroot()
                row = root.find(".//object[@id='row_language']")
                self.assertIsNotNone(row)
                title = row.find("./property[@name='title']")
                self.assertEqual(title.text, "Language")
                self.assertEqual(title.attrib.get("translatable"), "yes")
                # The model is populated from catalogs at runtime, not template IDs.
                self.assertIsNone(row.find("./property[@name='model']"))
                self.assertEqual(row.findall(".//items/item"), [])
                notice = root.find(".//object[@id='row_language_restart']")
                self.assertIsNotNone(notice)
                self.assertEqual(notice.find("./property[@name='visible']").text.lower(), "false")

    def test_real_sources_only(self):
        self.assertEqual(len(self.paths), len(set(self.paths)))
        for relative in self.paths:
            with self.subTest(source=relative):
                path = ROOT / relative
                self.assertTrue(path.is_file())
                self.assertIn(path.suffix, (".c", ".h", ".ui"))
                self.assertNotIn("build", path.parts[:-1])
                self.assertNotIn("smashed-pumpkin-resources.c", relative)

    def test_static_and_dynamic_extraction(self):
        for message in ("Overview", "Install", "Stop", "Force Stop...",
                        "Not installed", "Build %s", "Open", "Quit", "Language",
                        "System language", "Language change saved", "General settings saved"):
            with self.subTest(message=message):
                self.assertIn(f'msgid "{message}"', self.pot)
        self.assertIn("#, c-format", self.pot)
        self.assertIn("Translators: %s is a display date/time", self.pot)

    def test_full_dynamic_messages_and_plural_extraction(self):
        for message in ("Share your feedback", "World and dimension", "Kick %s?",
                        "Ban %s?", "by %s", "Granted ADMIN to %s", "Removed ADMIN from %s",
                        "%llud %lluh", "%lluh %llum", "%llum %llus", "%llus",
                        "%llus ago", "%llum ago", "%lluh ago", "%llud ago", "%lluy ago",
                        "%llds ago", "%lldm %02llds ago"):
            with self.subTest(message=message):
                self.assertIn(f'msgid "{message}"', self.pot)
        for singular, plural in (("Page %u · %u result", "Page %u · %u results"),
                                 ("Terrain / %u chunk", "Terrain / %u chunks"),
                                 ("%.0f block", "%.0f blocks"),
                                 ("%u online  ·  %u known player", "%u online  ·  %u known players")):
            with self.subTest(singular=singular):
                self.assertIn(f'msgid "{singular}"\nmsgid_plural "{plural}"', self.pot)
        # GLib's adjacent G_GUINT64_FORMAT macro is not understood by xgettext:
        # visible formats use complete literal C99 formats and explicit casts.
        self.assertNotIn('msgid "%"\n', self.pot)

    def test_machine_tokens_are_not_catalog_entries(self):
        for message in ("Smashed Pumpkin", "Server process exited",
                        "Auto-restart scheduled", "HTTP status 401",
                        "minecraft:overworld", "session-%Y-%m-%d_%H-%M-%S.log",
                        "Permission denied", "approved: false", "Failed to load plugin",
                        "different iteration of the API", "Server is now running",
                        "Plugin %s %s successfully", "Plugin %s is %sloaded",
                        "Failed to %s plugin %s:", "fs.write.data", "default_level_name",
                        "list uuids", "tps", "ban", "kick", "op", "deop"):
            with self.subTest(message=message):
                self.assertNotIn(f'msgid "{message}"', self.pot)


if __name__ == "__main__":
    unittest.main()
