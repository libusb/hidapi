"""Validate submission gates without running or uploading an analysis."""

import contextlib
import io
import pathlib
import tempfile
import unittest
from unittest.mock import patch

import merge


VERSION = "version 2024.12.1 on test\nInternal version numbers: test-build\n"
SOURCES = {
    "windows": ["C:/src/windows/hid.c", "C:/src/hidtest/test.c"],
    "macos": ["/mac/src/mac/hid.c"],
    "linux": ["/linux/src/linux/hid.c", "/linux/src/libusb/hid.c",
              "/linux/src/hidtest/test.c", "/linux/src/hidtest/test.c"],
}


class MergeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.captures = self.root / "captures"
        self.target = self.root / "combined"
        self.sources = {key: value.copy() for key, value in SOURCES.items()}
        self.calls = []
        self.drop_duplicate = False
        for platform in SOURCES:
            capture = self.captures / f"coverity-{platform}"
            (capture / "cov-int").mkdir(parents=True)
            (capture / "coverity-version.txt").write_text(VERSION)

    def fake_cov(self, *args):
        self.calls.append(args)
        if args == ("--ident",):
            return VERSION
        idir, command = args[1:3]
        if command == "list":
            if idir == self.target:
                paths = [path for values in self.sources.values() for path in values]
                if self.drop_duplicate:
                    paths = list(dict.fromkeys(paths))
            else:
                paths = self.sources[idir.parent.name.removeprefix("coverity-")]
            return "".join(f"Translation unit:\n{i} -> {path}\n" for i, path in enumerate(paths, 1))
        return ""

    def run_merge(self):
        with patch.object(merge, "cov", self.fake_cov), \
                patch.object(pathlib.Path, "write_text") as report, \
                contextlib.redirect_stdout(io.StringIO()):
            merge.merge(self.captures, self.target)
            return report.call_args.args[0]

    def test_preserves_repeated_sources(self):
        report = self.run_merge()
        self.assertIn('"merged": 7', report)
        self.assertEqual(sum(call[2] == "add" for call in self.calls if len(call) > 2), 2)

    def test_rejects_version_mismatch(self):
        (self.captures / "coverity-macos/coverity-version.txt").write_text(
            VERSION.replace("test-build", "other-build"))
        with self.assertRaisesRegex(RuntimeError, "version mismatch"):
            self.run_merge()

    def test_rejects_empty_capture(self):
        self.sources["windows"] = []
        with self.assertRaisesRegex(RuntimeError, "Empty or incomplete"):
            self.run_merge()

    def test_rejects_missing_backend(self):
        self.sources["linux"].remove("/linux/src/libusb/hid.c")
        with self.assertRaisesRegex(RuntimeError, "Missing backend"):
            self.run_merge()

    def test_rejects_recoverable_errors(self):
        self.sources["linux"][1] += " (recoverable errors)"
        with self.assertRaisesRegex(RuntimeError, "Empty or incomplete"):
            self.run_merge()

    def test_rejects_lost_duplicate(self):
        self.drop_duplicate = True
        with self.assertRaisesRegex(RuntimeError, "differ"):
            self.run_merge()

    def test_rejects_invalid_version(self):
        with self.assertRaisesRegex(ValueError, "version output"):
            merge.version("unrecognized output")

    def test_accepts_tool_prefixed_version(self):
        self.assertEqual(merge.version("Coverity tool " + VERSION), merge.version(VERSION))

    def test_accepts_windows_text(self):
        (self.captures / "coverity-windows/coverity-version.txt").write_bytes(
            ("\ufeff" + VERSION.replace("\n", "\r\n")).encode("utf-8"))
        self.sources["windows"] = [path.replace("/", "\\") for path in SOURCES["windows"]]
        self.assertIn('"merged": 7', self.run_merge())

    def test_rejects_unrecoverable_errors(self):
        self.sources["linux"][1] += " (unrecoverable errors)"
        with self.assertRaisesRegex(RuntimeError, "Empty or incomplete"):
            self.run_merge()

    def test_rejects_missing_metadata(self):
        (self.captures / "coverity-macos/coverity-version.txt").unlink()
        with self.assertRaises(FileNotFoundError):
            self.run_merge()


if __name__ == "__main__":
    unittest.main()
