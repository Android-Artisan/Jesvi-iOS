import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('prepare_m0', Path(__file__).parents[1] / 'prepare-m0.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class FallbackTests(unittest.TestCase):
    def setUp(self):
        self.original = [(b'loader.efi'.ljust(16, b'\0'), 0, b'original loader'),
                         (b'cmdline'.ljust(16, b'\0'), 7, b'original arguments\0')]

    def test_preflight_retains_exact_installed_loader_and_arguments(self):
        loader = b'MZ CELLULAR MEMORY PREFLIGHT'
        changed = prepare.replace_loader(self.original, loader, True)
        self.assertEqual(changed[0][2], loader)
        self.assertEqual(changed[1], (b'fallback.efi'.ljust(16, b'\0'), 0, b'original loader'))
        self.assertEqual(changed[2], (b'probe.report'.ljust(16, b'\0'), 0, bytes(16384)))
        self.assertEqual(changed[3], self.original[1])

    def test_reject_missing_or_inappropriate_fallback(self):
        with self.assertRaises(ValueError):
            prepare.replace_loader(self.original, b'MZ CELLULAR MEMORY PREFLIGHT')
        with self.assertRaises(ValueError):
            prepare.replace_loader(self.original, b'MZ normal loader', True)

    def test_reject_recursive_or_ambiguous_backup(self):
        for records in ([], self.original + self.original[:1],
                        self.original + [(b'fallback.efi'.ljust(16, b'\0'), 0, b'old')]):
            with self.subTest(records=records), self.assertRaises(ValueError):
                prepare.replace_loader(records, b'MZ CELLULAR MEMORY PREFLIGHT', True)


if __name__ == '__main__':
    unittest.main()
