"""Regression evidence for the latest public YSM animation contract."""
import json
import hashlib
import pathlib
import re
import subprocess
import tempfile
import unittest
import zipfile

import test_molang
from test_import import EXE, PROJECT, SAMPLE


class AnimationCompatibilityTests(unittest.TestCase):
    animation = test_molang.MolangTests.animation

    def test_identical_axis_programs_evaluate_again_with_each_lower_channel(self):
        source = 'v.n=(v.n??0)+1;return this+v.n;'
        report = self.animation({
            'pre_parallel0': {'bones': {'a': {'position': [10,20,30]}, 'b': {'position': [40,50,60]}}},
            'idle': {'bones': {'a': {'position': [source]*3}, 'b': {'position': [source]*3}}}
        })
        self.assertEqual(report['expression_failures'], 0)
        self.assertEqual(report['variables']['variable.n'], 6)
        self.assertEqual(report['pose']['a']['position'], [11,22,33])
        self.assertEqual(report['pose']['b']['position'], [44,55,66])

    def test_literal_vector_preserves_partial_budget_and_previous_failures(self):
        # The nested loops consume all but three, two, or one of the 200000 steps.
        for count, expected, failures in [(946, [2, 3, 4], 0), (947, [2, 3, 1], 1), (948, [2, 1, 1], 2)]:
            with self.subTest(count=count):
                burn = f'loop(194,loop(1024,0));loop({count},0);return 0;'
                report = self.animation({'idle': {'bones': {'a': {'position': burn}, 'b': {'scale': [2, 3, 4]}}}})
                self.assertEqual(report['pose']['b']['scale'], expected)
                self.assertEqual(report['expression_failures'], failures)
        report = self.animation({'idle': {'bones': {'a': {'position': 'math.pow(10,400)'},
                                                    'b': {'scale': [2, 3, 4]}}}})
        self.assertEqual(report['expression_failures'], 1)
        self.assertEqual(report['pose']['b']['scale'], [2, 3, 4])

    def test_legacy_vector_key_and_null_pre_post_preserve_the_authored_value(self):
        for key in [{'vector': [10, 0, 0], 'easing': 'linear'},
                    {'pre': None, 'post': [10, 0, 0]},
                    {'pre': [10, 0, 0], 'post': None}]:
            report = self.animation({'idle': {'bones': {'root': {'position': {'0': key}}}}})
            self.assertEqual(report['pose']['root']['position'], [10, 0, 0])
            self.assertEqual(report['compile_diagnostics'], [])

    def test_catmullrom_accepts_source_case_and_legacy_easing_spelling(self):
        for field in ['lerp_mode', 'easing']:
            keys = {str(i): {('vector' if field == 'easing' else 'post'): [value, 0, 0],
                             field: 'CaTmUlLrOm'} for i, value in enumerate([0, 10, 20, 0])}
            pose = self.animation({'idle': {'loop': True, 'bones': {'root': {'position': keys}}}}, 1.5)['pose']
            self.assertAlmostEqual(pose['root']['position'][0], 16.875)

    def test_keyframe_boundaries_do_not_evaluate_unused_endpoints(self):
        keys = {'0': ['v.begin+=1;return 2;', 0, 0],
                '1': {'pre': ['v.future+=1;return 10;', 0, 0],
                      'post': ['v.endpoint+=1;return 20;', 0, 0]}}
        clip = {'loop': 'hold_on_last_frame', 'bones': {'root': {'position': keys}}}
        for time in [0, 0.000001]:
            variables = self.animation({'idle': clip}, time)['variables']
            self.assertEqual(variables['variable.begin'], 1)
            self.assertNotIn('variable.future', variables)
        variables = self.animation({'idle': clip}, 0.999999)['variables']
        self.assertEqual(variables['variable.endpoint'], 1)
        self.assertNotIn('variable.future', variables)
        self.assertNotIn('variable.begin', variables)

    def test_catmullrom_evaluates_left_begin_end_right_in_source_order(self):
        keys = {str(i): {'post': ['v.order=v.order*10+' + str(i+1) + ';return 0;', 0, 0],
                         'lerp_mode': 'catmullrom'} for i in range(3)}
        clip = {'loop': True, 'bones': {'root': {'position': keys}}}
        self.assertEqual(self.animation({'idle': clip}, .5)['variables']['variable.order'], 1123)
        self.assertEqual(self.animation({'idle': clip}, 1.5)['variables']['variable.order'], 1233)

    def test_embedded_default_assets_match_official_265_release(self):
        embedded = (PROJECT / 'src/DefaultAnimations.inc').read_text(encoding='utf-8')
        actual = [json.loads(item) for item in re.findall(r'R"ysmdef\((.*?)\)ysmdef"', embedded)]
        self.assertEqual([len(clips) for clips in actual], [341, 8, 7])
        self.assertEqual(sum(map(len, actual[2].values())), 58)
        # Canonical JSON hashes from the three original default animation domains.
        checksums = [hashlib.sha256(json.dumps(clips, sort_keys=True, ensure_ascii=True,
                                              separators=(',', ':')).encode()).hexdigest() for clips in actual]
        self.assertEqual(checksums, [
            '7699130702247e40e706fa375faed7152d0ba201e184d5750bcbc2f556268acc',
            'f0c01f8764086194b5f04785bd9fe97e0eba63cd7fd872b8ce1ca03d3a0e2432',
            'd106667feddbaa91a2a2ca2ce333831df3437a95a3b2ce2ec69d69cf33ada93f'])

    def package_frames(self, walk, frames):
        with zipfile.ZipFile(SAMPLE) as source:
            content = {entry.filename: source.read(entry) for entry in source.infolist() if not entry.is_dir()}
        descriptor_name = next(name for name in content if name.endswith('ysm.json'))
        prefix = descriptor_name[:-8]
        descriptor = json.loads(content[descriptor_name].decode('utf-8-sig'))
        for filename in descriptor['files']['player']['animation'].values():
            name = prefix + filename
            document = json.loads(content[name].decode('utf-8-sig'))
            if 'walk' not in document['animations']:
                continue
            if walk is None:
                del document['animations']['walk']
            else:
                document['animations']['walk'] = walk
            content[name] = json.dumps(document).encode('utf-8')
        with tempfile.TemporaryDirectory() as directory:
            archive = pathlib.Path(directory) / 'fallback.zip'
            with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as target:
                for name, payload in content.items():
                    target.writestr(name, payload)
            run = subprocess.run([str(EXE), '--player-animations', str(archive), json.dumps({'frames': frames})],
                                 capture_output=True, encoding='utf-8', timeout=15)
        self.assertEqual(run.returncode, 0, run.stderr)
        report = json.loads(run.stdout)
        self.assertEqual(report['expression_failures'], 0)
        self.assertEqual(report['compile_diagnostics'], [
            'pre_parallel1 ' + bone + ': Molang at 4: Non-finite literal'
            for bone in ['Red_Knot', 'gold_bar', 'gold_bar2', 'Chest_red_knot']])
        return report['frames']

    def test_missing_walk_in_real_package_uses_the_default_stride(self):
        frames = self.package_frames(None, [{'time': t, 'state': 'walk'} for t in [0, .1]])
        self.assertEqual(frames[-1]['controllers']['coded']['player.main']['animation'], 'walk')
        self.assertAlmostEqual(frames[-1]['pose']['LeftLeg']['rotation'][0], -27.47911)

    def test_authored_walk_and_explicit_empty_walk_override_the_default(self):
        walk = {'loop': True, 'bones': {'LeftLeg': {'rotation': [77, 0, 0]}}}
        frames = self.package_frames(walk, [{'time': t, 'state': 'walk'} for t in [0, .2]])
        self.assertEqual(frames[-1]['pose']['LeftLeg']['rotation'][0], 77)
        frames = self.package_frames({}, [{'time': t, 'state': 'walk'} for t in [0, .2]])
        main = frames[-1]['controllers']['coded']['player.main']
        self.assertEqual(main['animation'], 'walk')
        self.assertEqual(frames[-1]['pose'].get('LeftLeg', {}).get('rotation', [0, 0, 0]), [0, 0, 0])

    def test_legacy_hand_loop_respects_fallback_provenance(self):
        clip = {'animation_length': .1, 'bones': {'root': {'position': [10, 0, 0]}}}
        for fallback in [False, True]:
            document = {'animations': {'hold_mainhand:empty': clip}, 'ysm_origin_version': '2.4.1',
                        'ysm_fallback_animations': ['hold_mainhand:empty'] if fallback else []}
            with tempfile.TemporaryDirectory() as directory:
                path = pathlib.Path(directory) / 'hand-loop.json'
                path.write_text(json.dumps(document), encoding='utf-8')
                run = subprocess.run([str(EXE), '--hand-animation-file', str(path),
                                      json.dumps({'frames': [{'time': 0}, {'time': .5}]})],
                                     capture_output=True, encoding='utf-8', timeout=5)
            self.assertEqual(run.returncode, 0, run.stderr)
            pose = json.loads(run.stdout)['frames'][-1]['pose']
            self.assertEqual('root' in pose, not fallback)
