"""Unit tests for prune-setup-ui.py (run: python3 -m unittest discover -s scripts/ci -p 'test_*.py')."""

import importlib.util
import json
import pathlib
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("prune_setup_ui", pathlib.Path(__file__).with_name("prune-setup-ui.py"))
prune_setup_ui = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prune_setup_ui)


class Prune(unittest.TestCase):
    def make_dist(self, clock_referenced=False):
        root = pathlib.Path(tempfile.mkdtemp())
        dist = root / "dist"
        assets = dist / "assets"
        assets.mkdir(parents=True)
        css = (
            "@font-face{font-family:Clock Roboto Flex;src:url(./clock-a.woff2)}"
            "@font-face{font-family:Roboto Flex;src:url(./roboto.woff2)}"
            ".clock-face{color:red}.ok{color:blue}"
            "@media (min-width:600px){.clock-tall{width:1px}.ok2{width:2px}}"
        )
        (assets / "app.css").write_text(css, encoding="utf-8")
        script = "const font = './roboto.woff2';" + ("const x = './clock-a.woff2';" if clock_referenced else "")
        (assets / "app.js").write_text(script, encoding="utf-8")
        (dist / "index.html").write_text('<link href="./assets/app.css"><script src="./assets/app.js"></script>', encoding="utf-8")
        (assets / "clock-a.woff2").write_bytes(b"clock")
        (assets / "roboto.woff2").write_bytes(b"roboto")
        (assets / "app.css.map").write_text("{}", encoding="utf-8")
        return dist, assets

    def test_clock_rules_fonts_and_maps_are_removed(self):
        dist, assets = self.make_dist()
        report = dist.parent / "report.json"
        result = prune_setup_ui.prune(dist, report)
        css = (assets / "app.css").read_text(encoding="utf-8")
        self.assertNotIn("clock", css.lower())
        self.assertIn(".ok{color:blue}", css)
        self.assertIn(".ok2{width:2px}", css)
        self.assertFalse((assets / "clock-a.woff2").exists())
        self.assertTrue((assets / "roboto.woff2").exists())
        self.assertFalse((assets / "app.css.map").exists())
        self.assertEqual(result["locales_removed"], 0)
        self.assertGreater(result["removed_bytes"], 0)
        self.assertGreater(result["rewritten_bytes"], 0)
        self.assertEqual(json.loads(report.read_text())["removed_bytes"], result["removed_bytes"])

    def test_a_clock_font_the_script_still_uses_is_kept(self):
        dist, assets = self.make_dist(clock_referenced=True)
        with self.assertRaises(SystemExit):
            prune_setup_ui.prune(dist)
        self.assertTrue((assets / "clock-a.woff2").exists())


if __name__ == "__main__":
    unittest.main()
