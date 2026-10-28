#!/usr/bin/env python3
"""Exercise soak.sh's production checker with process/thread samples."""

from pathlib import Path
import os
import subprocess
import sys
import unittest


SOURCE = Path(__file__).with_name("soak.sh").read_text()
# Execute the real awk program, so these cases also protect shell changes.
PROGRAM = SOURCE[SOURCE.index('awk -v actions='):].split("'\n", 1)[1].split(
    "' \"$samples\"", 1)[0]


class SoakCheckTest(unittest.TestCase):
    def check(self, early, late, succeeds, reason=""):
        rows = [early] * 16 + [late] * 4
        result = subprocess.run(
            ["awk", "-v", "actions=12", "-v", "opens=4", "-v", "errors=0",
             PROGRAM], input="\n".join(rows) + "\n", text=True,
            capture_output=True)
        self.assertEqual(result.returncode == 0, succeeds, result.stdout)
        self.assertIn("PASS soak" if succeeds else reason, result.stdout)
        return result.stdout

    def test_system_worker_growth_is_reported_without_becoming_a_leak(self):
        output = self.check("32 12 4", "33 13 5", True)
        self.assertIn("threads 8 -> 8", output)
        self.assertIn("total 12 -> 13, system workqueue 4 -> 5", output)

    def test_non_workqueue_growth_still_fails(self):
        self.check("32 12 4", "33 14 5", False, "threads grew")

    def test_growth_is_not_hidden_by_a_shrinking_system_pool(self):
        self.check("32 12 4", "33 12 3", False, "threads grew")

    def test_unknown_or_decoder_threads_remain_checked(self):
        self.check("32 8 0", "33 9 0", False, "threads grew")

    def test_existing_two_column_samples_keep_their_strict_check(self):
        self.check("32 8", "33 8", True)
        self.check("32 8", "33 9", False, "threads grew")

    def test_memory_growth_is_not_exempted(self):
        self.check("32 12 4", "90 13 5", False, "memory grew")

    @unittest.skipUnless(sys.platform == "darwin", "Darwin libproc sampler")
    def test_darwin_sampler_handles_a_process_without_a_workqueue(self):
        sampler = SOURCE.split('python3 - "$pid" <<\'PY\'\n', 1)[1].split(
            "\nPY", 1)[0]
        result = subprocess.run(
            [sys.executable, "-c", sampler, str(os.getpid())],
            text=True, capture_output=True, check=True)
        memory, total, workqueue = map(int, result.stdout.split())
        self.assertGreater(memory, 0)
        self.assertGreaterEqual(total, 1)
        self.assertGreaterEqual(workqueue, 0)
        self.assertLessEqual(workqueue, total)


if __name__ == "__main__":
    unittest.main()
