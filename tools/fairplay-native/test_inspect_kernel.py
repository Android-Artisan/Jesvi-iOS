"""Synthetic checks for address, instruction and Apple XML handling."""
import struct
import unittest

from inspect_kernel import branch_target, commands, decode_prelink, file_offset
from stage_native import resolve_target
from pack_prelink import metadata_xml, validate_parts


class InventoryTests(unittest.TestCase):
    def test_xml_references_preserve_dependency_values(self):
        result = decode_prelink(b'''<dict><key>original</key>
          <string ID="4">IOResources</string><key>ref</key><string IDREF="4"/>
          <key>address</key><integer>0xfffffff007004000</integer></dict>\0''')
        self.assertEqual(result['ref'], 'IOResources')
        self.assertEqual(result['address'], 0xfffffff007004000)

    def test_xml_cycle_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Cyclic'):
            decode_prelink(b'<string ID="1" IDREF="1"/>')

    def test_branches_use_signed_immediate(self):
        self.assertEqual(branch_target(0x94000004, 0x1000), 0x1010)
        self.assertEqual(branch_target(0x17ffffff, 0x1000), 0xffc)
        self.assertIsNone(branch_target(0xd65f03c0, 0x1000))

    def test_vm_mapping_rejects_zerofill_and_cross_boundary_reads(self):
        seg = dict(address=0x1000, size=0x1000, fileoff=0x40, filesize=16)
        self.assertEqual(file_offset([seg], 0x1008, 8), 0x48)
        for address, size in ((0xfff, 1), (0x1010, 1), (0x1008, 9)):
            with self.assertRaises(ValueError):
                file_offset([seg], address, size)

    def test_load_commands_reject_malformed_size(self):
        data = bytearray(40)
        struct.pack_into('<I', data, 0, 0xfeedfacf)
        struct.pack_into('<II', data, 16, 1, 8)
        struct.pack_into('<II', data, 32, 25, 0)
        with self.assertRaisesRegex(ValueError, 'Invalid load'):
            list(commands(data))

    def test_symbolic_relocation_preserves_small_addend(self):
        relocation = dict(target='0x1003', classification='named_kernel_symbol',
                          symbols=['_test'], symbol_addend=3)
        self.assertEqual(resolve_target(relocation, {'_test': 0x8000}, 0x4000), 0x8003)

    def test_component_relocation_preserves_relative_layout(self):
        relocation = dict(target='0x1000', classification='component')
        self.assertEqual(resolve_target(relocation, {}, 0x4000), 0x5000)

    def test_unknown_or_conflicting_imports_fail_closed(self):
        for relocation, names in (
            (dict(target='0x1000', classification='unresolved'), {}),
            (dict(target='0x1000', classification='named_kernel_symbol',
                  symbols=['_missing'], symbol_addend=0), {}),
            (dict(target='0x1000', classification='named_kernel_symbol',
                  symbols=['_a', '_b'], symbol_addend=0), {'_a': 0x8000, '_b': 0x9000}),
        ):
            with self.assertRaises(ValueError):
                resolve_target(relocation, names, 0x4000)

    def test_kernel_xml_roundtrip_retains_pointer_list_and_dependencies(self):
        original = dict(_PrelinkInfoDictionary=[dict(CFBundleIdentifier='com.test',
                        OSBundleLibraries={'com.apple.kpi.bsd': '8.0'}, OSKernelResource=True)],
                        _PrelinkLinkKASLROffsets=struct.pack('<III', 2, 0x100, 0x208))
        self.assertEqual(decode_prelink(metadata_xml(original)), original)

    def test_prelink_layout_accepts_gaps_and_rejects_overlap(self):
        parts = [(dict(address=0x1000, size=0x100), b''),
                 (dict(address=0x1200, size=0x100), b'')]
        validate_parts(parts, 0x1000, 0x2000)
        with self.assertRaises(ValueError):
            validate_parts(parts, 0x1001, 0x2000)
        with self.assertRaises(ValueError):
            validate_parts(parts + [(dict(address=0x1080, size=0x100), b'')], 0x1000, 0x2000)


if __name__ == '__main__':
    unittest.main()
