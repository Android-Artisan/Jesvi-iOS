#!/usr/bin/env python3
"""Fixup safety tests, without firmware or device dependencies."""
import runpy
import unittest
from pathlib import Path

M = runpy.run_path(str(Path(__file__).with_name('aux-inventory.py')))


class Fixups(unittest.TestCase):
    def test_external_bus_is_symbolic(self):
        base = {'/__symbols__': {'bus': b'/hsi2c@10560000\0'}}
        overlay = {'/__fixups__': {'bus': b'/fragment@16:target:0\0'},
                   '/fragment@16': {'target': b'\xff'*4}}
        self.assertEqual(M['fixups'](base, overlay)[('/fragment@16', 'target', 0)],
                         {'symbol': 'bus', 'base_path': '/hsi2c@10560000'})

    def test_missing_symbol_preserved(self):
        overlay = {'/__fixups__': {'unknown': b'/f:target:0\0'},
                   '/f': {'target': b'\xff'*4}}
        self.assertIsNone(M['fixups']({}, overlay)[('/f', 'target', 0)]['base_path'])

    def test_invalid_offsets_rejected(self):
        for offset in (-4, 1, 4):
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                M['fixups']({}, {'/__fixups__': {'bus': f'/f:target:{offset}\0'.encode()},
                                  '/f': {'target': b'\xff'*4}})


if __name__ == '__main__':
    unittest.main()
