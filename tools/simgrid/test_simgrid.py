"""simgrid's action parsing, and (when the sims are built) a run end to end.

    python3 -m unittest discover -s tools/simgrid
The end-to-end tests need `simgrid.py build --boards 154,round` first.
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import engine as eng  # noqa: E402
import simgrid  # noqa: E402
from profiles import PROFILES, select  # noqa: E402


class ParseTest(unittest.TestCase):
    def test_positional_and_keyword_words(self):
        self.assertEqual(simgrid.parse_action(["tap", "154", "120", "y=80", "hold_ms=900"]),
                         ("tap", ["154", "120"], {"y": "80", "hold_ms": "900"}))

    def test_json_word_stays_positional(self):
        self.assertEqual(simgrid.parse_action(["serial", '{"a=1"}']), ("serial", ['{"a=1"}'], {}))

    def test_coerce_follows_the_default(self):
        self.assertIs(eng.coerce("false", True), False)
        self.assertEqual(eng.coerce("1500", 120), 1500)
        self.assertEqual(eng.coerce("0.5", 1.0), 0.5)
        self.assertEqual(eng.coerce("42.5", None), 42.5)
        self.assertEqual(eng.coerce("Aug 15", None), "Aug 15")

    def test_every_action_is_described(self):
        d = eng.describe()
        self.assertEqual(set(d), set(eng.ACTIONS))
        for name, (_, doc) in d.items():
            self.assertTrue(doc, f"{name} has no docstring")


class ProfilesTest(unittest.TestCase):
    def test_ids_are_unique(self):
        ids = [p.id for p in PROFILES]
        self.assertEqual(len(ids), len(set(ids)))

    def test_select(self):
        self.assertEqual([p.id for p in select("154,round")], ["154", "round"])
        by_size = select("240x240")           # every cell of that size
        self.assertIn("154", [p.id for p in by_size])
        self.assertTrue(all(p.size == "240x240" for p in by_size))
        self.assertTrue(all(p.on_main for p in select("main")))
        with self.assertRaises(ValueError):
            select("nope")

    def test_round_cells_are_square(self):
        for p in PROFILES:
            if p.round:
                self.assertEqual(p.w, p.h)


BUILT = all(os.path.exists(eng.program_path(s)) for s in ("240x240", "466x466"))


@unittest.skipUnless(BUILT, "sims not built (simgrid.py build --boards 154,round)")
class RunTest(unittest.TestCase):
    SCRIPT = """
        step 3000
        beat attention
        step 2500
        shot {out}/a.png
        tap 154 120 120 hold_ms=200
        link false
        step 500
        shot {out}/b.png board=round
    """

    def run_once(self, out):
        e = eng.Engine("154,round", log_dir=out)
        try:
            simgrid.run_script(e, self.SCRIPT.format(out=out).splitlines(), log=lambda *_: None)
            return e.now
        finally:
            e.close()
            for sim in e.sims:
                self.assertTrue(sim.io.closed)
                self.assertEqual(sim.sock.fileno(), -1)

    def test_same_script_same_pixels(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            self.assertEqual(self.run_once(a), 6200)
            self.run_once(b)
            for name in ("a.png", "b.png"):
                with open(os.path.join(a, name), "rb") as fa, open(os.path.join(b, name), "rb") as fb:
                    self.assertEqual(fa.read(), fb.read(), name)

    def test_codex_pace_tracks_the_actual_window(self):
        with tempfile.TemporaryDirectory() as out:
            e = eng.Engine("154", log_dir=out)
            try:
                e.liveliness(1)
                e.payload(os=31, osr=150, pm=1)
                e.step(1500)
                e.payload(m="Codex needs you!", ax=1)
                e.step(4000)
                windows = [
                    dict(os=31, osr=150, ow=None, owr=None),
                    dict(os=None, osr=None, ow=31, owr=5040),
                    dict(os=31, osr=150, ow=10, owr=5040),
                ]
                for window in windows:
                    with self.subTest(window=window):
                        e.payload(**window)
                        e.step(500)
                        image = e.frame("154")
                        # Locate the first bar by its violet fill; 31% is
                        # longer than the second bar's 10% in the dual view.
                        rows = {y: sum(b > r + 15 and b > g + 15
                                       for r, g, b in (image.getpixel((x, y)) for x in range(18, 222)))
                                for y in range(60, 198)}
                        y = max(rows, key=rows.get)
                        self.assertGreater(rows[y], 40, "the Codex fill must be visible")
                        marker = [x for x in range(18, 222) if min(image.getpixel((x, y))) > 160]
                        self.assertTrue(marker, "the pace marker must be visible")
                        self.assertAlmostEqual(sum(marker) / len(marker), 120, delta=2,
                                               msg="half a window must put the marker at the centre")
            finally:
                e.close()

    def test_fable_overlay_stays_on_claude_when_turning_pages(self):
        with tempfile.TemporaryDirectory() as out:
            e = eng.Engine("154", log_dir=out)
            try:
                e.liveliness(1)
                e.payload(w=80, wf=60, fc="ff00ff", os=31, osr=150, ow=42, owr=5040)
                e.step(1500)

                def overlay_pixels():
                    image = e.frame("154")
                    return sum(r > 180 and g < 70 and b > 180
                               for r, g, b in (image.getpixel((x, y))
                                              for y in range(image.height)
                                              for x in range(image.width)))

                self.assertGreater(overlay_pixels(), 100, "Claude must show the Fable strip")
                e.beat("codex")
                e.step(4000)
                self.assertEqual(overlay_pixels(), 0, "Codex must hide Claude's Fable strip")
                e.payload(wf=80, fc="ff00ee")
                e.step(500)
                self.assertEqual(overlay_pixels(), 0, "a live Fable update must stay hidden on Codex")
                e.beat("codex")
                e.step(4000)
                self.assertGreater(overlay_pixels(), 100, "returning to Claude must restore Fable")
                e.payload(acct="week")
                e.step(500)
                self.assertGreater(overlay_pixels(), 100, "a weekly-only Claude account still has Fable")
                e.beat("codex")
                e.step(4000)
                self.assertEqual(overlay_pixels(), 0, "Codex must also hide the first-card overlay")
            finally:
                e.close()


if __name__ == "__main__":
    unittest.main()
