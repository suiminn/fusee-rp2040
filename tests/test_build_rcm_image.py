import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from build_rcm_image import DEFAULT_LAYOUT, RcmImageError, align_up, build_rcm_image, format_c_header


class BuildRcmImageTests(unittest.TestCase):
    def test_builds_expected_layout(self):
        layout = DEFAULT_LAYOUT
        payload = bytes((index % 251 for index in range(0x5000)))
        intermezzo = bytes(range(32))

        image = build_rcm_image(payload, intermezzo, layout)

        payload_stream_offset = layout.command_payload_offset + (
            layout.payload_start_addr - layout.rcm_payload_addr
        )
        spray_stream_offset = layout.command_payload_offset + (
            layout.stack_spray_start - layout.rcm_payload_addr
        )
        spray_len = layout.stack_spray_end - layout.stack_spray_start
        payload_before_spray = layout.stack_spray_start - layout.payload_start_addr
        unpadded_size = (
            layout.command_payload_offset
            + (layout.payload_start_addr - layout.rcm_payload_addr)
            + payload_before_spray
            + spray_len
            + (len(payload) - payload_before_spray)
        )

        self.assertEqual(int.from_bytes(image[:4], "little"), layout.rcm_max_length)
        self.assertEqual(
            image[layout.command_payload_offset : layout.command_payload_offset + len(intermezzo)],
            intermezzo,
        )
        self.assertEqual(image[payload_stream_offset : payload_stream_offset + payload_before_spray], payload[:payload_before_spray])

        spray_words = image[spray_stream_offset : spray_stream_offset + spray_len]
        self.assertEqual(
            spray_words,
            layout.rcm_payload_addr.to_bytes(4, "little") * (spray_len // 4),
        )
        self.assertEqual(
            image[spray_stream_offset + spray_len : spray_stream_offset + spray_len + 16],
            payload[payload_before_spray : payload_before_spray + 16],
        )
        self.assertEqual(len(image), align_up(unpadded_size, layout.usb_packet_size))

    def test_rejects_images_over_rcm_max(self):
        payload = bytes(DEFAULT_LAYOUT.rcm_max_length)
        intermezzo = b"intermezzo"

        with self.assertRaises(RcmImageError):
            build_rcm_image(payload, intermezzo)

    def test_rejects_intermezzo_overlap(self):
        layout = DEFAULT_LAYOUT
        intermezzo = bytes(layout.payload_start_addr - layout.rcm_payload_addr + 1)

        with self.assertRaises(RcmImageError):
            build_rcm_image(b"", intermezzo)

    def test_formats_header_with_metadata(self):
        image = build_rcm_image(b"payload", b"mezzo")
        header = format_c_header(image, Path("payload.bin"), Path("intermezzo.bin"), "test_image", "payloads")

        self.assertIn("#define RCM_IMAGE_LEN", header)
        self.assertIn("const uint8_t __in_flash(\"payloads\") test_image[]", header)
        self.assertIn("/* payload: payload.bin */", header)


if __name__ == "__main__":
    unittest.main()
