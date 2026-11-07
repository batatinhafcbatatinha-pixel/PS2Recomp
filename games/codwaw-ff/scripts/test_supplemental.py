"""Read-only checks against private analysis inputs and generated supplements."""
import pathlib
import re
import struct
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent

class SupplementalTests(unittest.TestCase):
    def test_original_range_is_unchanged(self):
        original = (ROOT / "analysis/disc/SLUS_217.46").read_bytes()
        selected = (ROOT / "analysis/function-1c90b0.elf").read_bytes()
        self.assertEqual(selected[128:128 + 776], original[128 + 0xc90b0:128 + 0xc90b0 + 776])
        self.assertEqual(struct.unpack_from("<I", selected, 24)[0], 0x1c90b0)

    def test_overlay_capture_matches_selected_code(self):
        capture = (ROOT / "analysis/missing-code-9e0000.bin").read_bytes()
        selected = (ROOT / "analysis/mpeg-relocated.elf").read_bytes()
        header = capture.find(b"SNR2")
        self.assertGreaterEqual(header, 0)
        delta = 0x9e0000 + header - 0x9e7480
        for offset in range(0, 62016, 4):
            old = struct.unpack_from("<I", selected, 128 + offset)[0]
            new = struct.unpack_from("<I", capture, header + 128 + offset)[0]
            if old == new:
                continue
            self.assertEqual(old >> 26, new >> 26)
            if old >> 26 in (2, 3):
                self.assertEqual((new - old) & 0x3ffffff, (delta // 4) & 0x3ffffff)
            else:
                self.assertIn(old >> 26, (9, 0x23, 0x2b, 0x39))
                self.assertEqual((new - old) & 0xffff, delta & 0xffff)

    def test_overlay_entrypoints_and_guards(self):
        directory = ROOT / "project/supplemental-mpeg"
        registration = (directory / "register_functions.cpp").read_text()
        integrated = (directory / "integrated.inc").read_text()
        entries = re.findall(r"= (sub_\w+); // (0x[0-9a-fA-F]+)", registration)
        self.assertEqual(len(entries), 2644)
        for function, address in entries:
            self.assertIn(f"runtime.registerFunction(codWawMpegAddress({address}u), checked_{function})", integrated)
            self.assertIn(f"static void checked_{function}(", integrated)
            self.assertGreaterEqual(int(address, 0), 0x9e7500)
            self.assertLess(int(address, 0), 0x9f6740)

    def test_relocated_immediates_are_not_frozen(self):
        capture = (ROOT / "analysis/missing-code-9e0000.bin").read_bytes()
        baseline = (ROOT / "analysis/mpeg-relocated.elf").read_bytes()
        header = capture.find(b"SNR2")
        blocks = {}
        for source in (ROOT / "project/supplemental-mpeg").glob("sub_*.cpp"):
            body = source.read_text()
            instructions = list(re.finditer(r"// (0x[0-9a-f]+): 0x[0-9a-f]+\s", body))
            for index, instruction in enumerate(instructions):
                end = instructions[index + 1].start() if index + 1 < len(instructions) else len(body)
                blocks.setdefault(int(instruction[1], 0), "")
                blocks[int(instruction[1], 0)] += body[instruction.end():end]
        for offset in range(0, 62016, 4):
            old = struct.unpack_from("<I", baseline, 128 + offset)[0]
            new = struct.unpack_from("<I", capture, header + 128 + offset)[0]
            if old == new or old >> 26 not in (9, 0x23, 0x2b, 0x39):
                continue
            address = 0x9e7500 + offset
            body = blocks[address]
            dynamic_operand = f"Ps2FastRead32(rdram, codWawMpegAddress({address:#x}u))" in body
            dynamic_address = re.search(r"FAST_\w+\(codWawMpegAddress\(", body)
            self.assertTrue(dynamic_operand or dynamic_address, hex(address))

if __name__ == "__main__":
    unittest.main()
