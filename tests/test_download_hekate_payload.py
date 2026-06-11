import hashlib
import os
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from download_hekate_payload import HekateDownloadError, download_file, find_standard_payload_asset, verify_digest


class DownloadHekatePayloadTests(unittest.TestCase):
    def test_finds_standard_payload_for_release_tag(self):
        release = {
            "tag_name": "v6.5.2",
            "assets": [
                {"name": "hekate_ctcaer_6.5.2_Nyx_1.9.2.zip", "browser_download_url": "https://example.invalid/nyx.zip"},
                {"name": "hekate_ctcaer_6.5.2.bin", "browser_download_url": "https://example.invalid/hekate.bin"},
            ],
        }

        asset = find_standard_payload_asset(release)

        self.assertEqual(asset["name"], "hekate_ctcaer_6.5.2.bin")
        self.assertEqual(asset["browser_download_url"], "https://example.invalid/hekate.bin")

    def test_rejects_release_without_standard_payload(self):
        release = {
            "tag_name": "v6.5.2",
            "assets": [{"name": "hekate_ctcaer_6.5.2_Nyx_1.9.2.zip"}],
        }

        with self.assertRaisesRegex(HekateDownloadError, "hekate_ctcaer_6.5.2.bin"):
            find_standard_payload_asset(release)

    def test_verifies_sha256_digest(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            payload = Path(temp_dir) / "payload.bin"
            payload.write_bytes(b"hekate")
            digest = hashlib.sha256(b"hekate").hexdigest()

            verify_digest(payload, f"sha256:{digest}")

    def test_rejects_sha256_digest_mismatch(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            payload = Path(temp_dir) / "payload.bin"
            payload.write_bytes(b"hekate")

            with self.assertRaisesRegex(HekateDownloadError, "digest mismatch"):
                verify_digest(payload, "sha256:000000")

    def test_download_keeps_cached_payload_on_digest_mismatch(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "source.bin"
            output = Path(temp_dir) / "payload.bin"
            source.write_bytes(b"bad payload")
            output.write_bytes(b"old payload")

            with self.assertRaisesRegex(HekateDownloadError, "digest mismatch"):
                download_file(source.as_uri(), output, digest="sha256:000000")

            self.assertEqual(output.read_bytes(), b"old payload")

    def test_download_leaves_cached_payload_when_content_matches(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "source.bin"
            output = Path(temp_dir) / "payload.bin"
            source.write_bytes(b"payload")
            output.write_bytes(b"payload")
            os.utime(output, (1, 1))
            digest = hashlib.sha256(b"payload").hexdigest()

            updated = download_file(source.as_uri(), output, digest=f"sha256:{digest}")

            self.assertFalse(updated)
            self.assertEqual(output.read_bytes(), b"payload")
            self.assertGreater(output.stat().st_mtime, 1)


if __name__ == "__main__":
    unittest.main()
