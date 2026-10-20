"""Exercise the real Linux binary without DLSS inference or Python GPU packages."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import unittest


class LinuxWorkerTests(unittest.TestCase):
    def run_worker(self, *args, data=None):
        return subprocess.run(
            [str(self.worker), *args], input=data, capture_output=True,
            cwd=self.worker.parent, timeout=30,
        )

    def test_invalid_setup_returns_binary_error(self):
        headers = [b"", b"\0" * 4]
        for values in [
            (0, 64, 64, 3, 1),
            (0x31534746, 0, 64, 3, 1),
            (0x31534746, 64, 0, 3, 1),
            (0x31534746, 8193, 64, 3, 1),
            (0x31534746, 8192, 8192, 3, 1),
            (0x31534746, 64, 64, 0, 1),
            (0x31534746, 64, 64, 3, 0),
            (0x31534746, 64, 64, 3, 2),
        ]:
            headers.append(struct.pack("<5I", *values))
        for header in headers:
            with self.subTest(header=header):
                result = self.run_worker("--serve", data=header)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, struct.pack("<4I", 0x31524746, 1, 0, 0))
                self.assertRegex(result.stderr, b"Invalid setup|Truncated worker input")

    def test_invalid_arguments_do_not_pollute_stdout(self):
        for args in [(), ("--unknown",), ("--probe", "--serve")]:
            with self.subTest(args=args):
                result = self.run_worker(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b"")
                self.assertIn(b"Usage:", result.stderr)

    def test_capability_probe_and_unsupported_device_failure(self):
        result = self.run_worker("--probe")
        capabilities = json.loads(result.stdout)
        self.assertIs(capabilities["available"], result.returncode == 0)
        self.assertEqual(capabilities["worker_version"], 2)
        self.assertEqual(capabilities["multi_frame_count_max"], 1)
        if not capabilities["available"]:
            self.assertTrue(capabilities["reason"])
            result = self.run_worker(
                "--serve", data=struct.pack("<5I", 0x31534746, 64, 64, 3, 1),
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, struct.pack("<4I", 0x31524746, 1, 0, 0))
            self.assertTrue(result.stderr)

    def test_vulkan_transfers(self):
        if not self.transfers:
            self.skipTest("Pass --transfers to exercise Vulkan (no DLSS)")
        result = self.run_worker("--self-test")
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        self.assertIn(b"DLSS was not evaluated", result.stdout)
        self.assertNotIn(b"Validation Error", result.stdout + result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("worker", type=Path)
    parser.add_argument("--transfers", action="store_true")
    args = parser.parse_args()
    LinuxWorkerTests.worker = args.worker.resolve(strict=True)
    LinuxWorkerTests.transfers = args.transfers
    if not os.access(LinuxWorkerTests.worker, os.X_OK):
        parser.error("Worker must be executable")
    unittest.main(argv=[__file__], verbosity=2)
