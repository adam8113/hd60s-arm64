#!/usr/bin/env python3
"""Hardware-free tests for tint_bisect_map against data/cold-init-replay.txt."""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import tint_bisect_map as tbm


class TintBisectMapTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.path = tbm.default_replay_path()
        if not cls.path.is_file():
            raise unittest.SkipTest(f"missing {cls.path}")
        cls.ops = tbm.parse_ops(cls.path)

    def test_op_count_matches_reporter_summary(self):
        self.assertEqual(len(self.ops), 9966)

    def test_recommended_stops_in_range_and_sorted(self):
        prev = 0
        for name, n, note in tbm.RECOMMENDED_STOPS:
            self.assertGreaterEqual(n, 1, name)
            self.assertLessEqual(n, len(self.ops), name)
            self.assertGreaterEqual(n, prev, name)
            self.assertTrue(note)
            prev = n
        self.assertEqual(tbm.RECOMMENDED_STOPS[-1][0], "full")
        self.assertEqual(tbm.RECOMMENDED_STOPS[-1][1], len(self.ops))

    def test_host_setup_is_first_nine(self):
        self.assertEqual(tbm.RECOMMENDED_STOPS[0][:2], ("host_setup", 9))
        expect = [
            (0x03, 0x0004),
            (0x01, 0x0014),
            (0x01, 0x001D),
            (0x01, 0x0019),
            (0x01, 0x0010),
            (0x09, 0x0001),
            (0x30, 0x0000),
            (0x03, 0x0030),
            (0x03, 0x0031),
        ]
        for i, (breq, wvalue) in enumerate(expect):
            op = self.ops[i]
            self.assertEqual(op["breq"], breq)
            self.assertEqual(op["wvalue"], wvalue)

    def test_wake_stop_includes_c2(self):
        wake_n = dict((n, v) for n, v, _ in tbm.RECOMMENDED_STOPS)["wake"]
        c2 = [o for o in self.ops[:wake_n] if o["breq"] == 0xC2]
        self.assertEqual(len(c2), 1)
        self.assertEqual(c2[0]["op"], 11)

    def test_hdmi_and_matrix_boundaries(self):
        by_name = {n: v for n, v, _ in tbm.RECOMMENDED_STOPS}
        before = by_name["before_hdmi_tx"]
        self.assertNotEqual(self.ops[before - 1]["wvalue"], 0x5098)
        self.assertEqual(self.ops[before]["wvalue"], 0x5098)  # op 705 is index 704
        self.assertEqual(self.ops[by_name["hdmi_tx1"] - 1]["wvalue"], 0x5098)

        before_m = by_name["before_matrix"]
        self.assertFalse(
            self.ops[before_m - 1]["data"]
            and 0x93 <= self.ops[before_m - 1]["data"][0] <= 0xAA
            and self.ops[before_m - 1]["wvalue"] == 0x509C
        )
        first_m = self.ops[before_m]  # op 979
        self.assertEqual(first_m["wvalue"], 0x509C)
        self.assertTrue(0x93 <= first_m["data"][0] <= 0xAA)

    def test_resolve_stop_name_and_number(self):
        self.assertEqual(tbm.resolve_stop("wake", len(self.ops)), 15)
        self.assertEqual(tbm.resolve_stop("20", len(self.ops)), 20)
        self.assertEqual(tbm.resolve_stop("99999", len(self.ops)), len(self.ops))
        with self.assertRaises(ValueError):
            tbm.resolve_stop("0", len(self.ops))
        with self.assertRaises(ValueError):
            tbm.resolve_stop("nope", len(self.ops))

    def test_describe_op_tags(self):
        c2 = self.ops[10]
        self.assertIn("[wake/reset]", tbm.describe_op(c2))
        hdmi = next(o for o in self.ops if o["wvalue"] == 0x5098)
        self.assertIn("[hdmi-tx]", tbm.describe_op(hdmi))
        matrix = next(
            o
            for o in self.ops
            if o["wvalue"] == 0x509C and o["data"] and 0x93 <= o["data"][0] <= 0xAA
        )
        self.assertIn("[colour-matrix]", tbm.describe_op(matrix))

    def test_skips_standard_descriptors(self):
        # Craft a tiny seq file: GET_DESCRIPTOR then a vendor op.
        body = "\n".join(
            [
                "IN|0x06|0x0100|0x0000|18|",
                "OUT|0xc0|0x5066|0x0000|3|9a 01 00",
            ]
        )
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "seq.txt"
            p.write_text(body + "\n")
            ops = tbm.parse_ops(p)
            self.assertEqual(len(ops), 1)
            self.assertEqual(ops[0]["breq"], 0xC0)

    def test_main_list_stops_exit_zero(self):
        self.assertEqual(tbm.main(["--list-stops"]), 0)
        self.assertEqual(tbm.main(["--stop-after", "host_setup"]), 0)
        self.assertEqual(tbm.main(["--describe-op", "11"]), 0)
        self.assertEqual(tbm.main(["--stop-after", "badname"]), 2)

    def test_start_cpp_stop_table_matches(self):
        text = (tbm.repo_root() / "src" / "start.cpp").read_text()
        for name, n, _ in tbm.RECOMMENDED_STOPS:
            needle = f'{{ "{name}",'
            self.assertIn(needle, text, name)
            # tolerate spacing in the C table initializer
            self.assertRegex(
                text,
                rf'\{{\s*"{name}",\s*{n}\s*,',
                msg=f"start.cpp stop {name} should be {n}",
            )


if __name__ == "__main__":
    unittest.main()
