#!/usr/bin/env python3
"""Tests of the pure parts of soak.py: python3 tools/test_soak.py"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import soak


class SoakTests(unittest.TestCase):
    def test_fdinfo_memory_in_kib_and_mib(self):
        text = "pos:\t0\ndrm-client-id:\t7\ndrm-memory-vram:\t2048 KiB\ndrm-memory-gtt:\t4 MiB\ndrm-memory-cpu:\t0 KiB\n"
        self.assertEqual(soak.parse_fdinfo_memory(text), {"vram": 2048, "gtt": 4096, "cpu": 0})
        self.assertEqual(soak.parse_fdinfo_memory("nothing"), {})

    def test_status_values(self):
        self.assertEqual(soak.parse_status_kb("VmRSS:\t  1234 kB\nVmHWM:\t 99 kB\n", "VmRSS"), 1234)
        self.assertEqual(soak.parse_status_kb("x", "VmRSS"), 0)

    def test_slope(self):
        self.assertAlmostEqual(soak.slope_per_hour([(0, 100), (1800, 110), (3600, 120)]), 20.0)
        self.assertEqual(soak.slope_per_hour([(0, 1), (1, 1)]), 0.0)
        self.assertAlmostEqual(soak.slope_per_hour([(0, 5), (10, 5), (20, 5)]), 0.0)

    def samples(self, rss, fds=20, vram=100.0, span=600):
        n = len(rss)
        return [{"t": span * i / (n - 1), "rss_mb": r, "fds": fds + (i if fds < 0 else 0), "vram_mb": vram} for i, r in enumerate(rss)]

    def test_a_steady_run_is_clean_and_a_growing_one_is_named(self):
        steady = {"index": 0, "item": "1", "exit": 0, "samples": self.samples([200 + (i % 2) for i in range(20)])}
        growing = {"index": 1, "item": "2", "exit": 0, "samples": self.samples([200 + 2 * i for i in range(20)])}
        self.assertEqual(soak.judge([steady]), [])
        leaks = soak.judge([steady, growing])
        self.assertEqual(len(leaks), 1)
        self.assertIn("2 (run 1)", leaks[0])
        self.assertIn("resident memory", leaks[0])

    def test_short_runs_are_not_judged_for_growth(self):
        short = {"index": 0, "item": "3", "exit": 0, "samples": self.samples([100, 300, 900], span=20)}
        self.assertEqual(soak.judge([short]), [])

    def test_descriptor_growth_and_crashes(self):
        fds = [{"t": 60.0 * i, "rss_mb": 100.0, "fds": 20 + 3 * i, "vram_mb": 10.0} for i in range(8)]
        leaks = soak.judge([{"index": 0, "item": "4", "exit": 0, "samples": fds}, {"index": 1, "item": "5", "exit": 139, "samples": []}])
        self.assertTrue(any("descriptors grew" in l for l in leaks))
        self.assertTrue(any("exited with 139" in l for l in leaks))

    def test_directory_bytes(self):
        import tempfile
        with tempfile.TemporaryDirectory(dir=os.path.expanduser("~/.cache")) as d:
            open(os.path.join(d, "a"), "wb").write(b"x" * 1000)
            os.mkdir(os.path.join(d, "sub"))
            open(os.path.join(d, "sub", "b"), "wb").write(b"y" * 24)
            self.assertEqual(soak.directory_bytes(d), 1024)


if __name__ == "__main__":
    unittest.main()
