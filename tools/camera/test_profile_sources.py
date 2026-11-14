import importlib.util
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('profile_sources',Path(__file__).with_name('prepare-profile-sources.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class ProfileSourceTests(unittest.TestCase):
    def test_shared_declaration_ignores_comments(self):
        self.assertEqual(m.declaration('struct x { int a; /* yes */ };','x'),m.declaration('struct x{int a;};','x'))
        self.assertNotEqual(m.declaration('struct x { int a; };','x'),m.declaration('struct x{long a;};','x'))
    def test_pinned_original_isp_and_front_default(self):
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp); report=m.prepare(out)
            for name in ('camera_isp.c','camera_isp.h','redwood_camera.h'):
                self.assertEqual((out/'original'/name).read_text(),m.original(name))
            source=(out/'original/camera_device.c').read_text()
            self.assertIn('if (!fp) return false;',source)
            self.assertIn('s->camera->sensor == 1 || front_experimental_profile()',source)
            self.assertEqual(report['default_front'],'original')
            # Old front's globals are confined to its original object; new ISP APIs use separate symbols.
            self.assertIn('extern int isp_black4[4]',(out/'original/camera_isp.h').read_text())
            self.assertIn('#define isp_measure experimental_isp_measure',(out/'experimental-symbols.h').read_text())

if __name__=='__main__':unittest.main()
