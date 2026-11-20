"""Run the same two-target controller fixture used in the Java 2.6.5 client."""
import json
from pathlib import Path
import subprocess
import unittest

from test_import import EXE


class ReplacedWorldInputsTests(unittest.TestCase):
    def test_world_inputs_select_the_registered_target_and_apply_its_clip(self):
        fixture = Path(__file__).with_name('fixtures') / 'WorldInputs.zip'
        for kind, entity_type in [('vehicle', 'boat'), ('projectile', 'arrow')]:
            frames = []
            expected = []
            for weather, label in enumerate(('clear', 'rain', 'thunder')):
                for exposed in (True, False):
                    entity = {'bedrock_weather': {'rain': float(weather > 0), 'lightning': float(weather == 2)},
                              'sky_visible': True, 'block_y': 70, 'blocking_height': 65 if exposed else 73}
                    state = label + ('_open' if exposed else '_roof')
                    for _ in range(2):
                        frames.append({'time': len(frames) / 10, 'entity': entity})
                    expected.append((len(frames) - 1, state, [weather, int(exposed), 1]))
            request = {'kind': kind, 'entity_type': entity_type, 'frames': frames}
            output = subprocess.check_output([str(EXE), '--target-animations', str(fixture), json.dumps(request)],
                                             encoding='utf-8')
            result = json.loads(output)
            self.assertEqual(result['target']['id'], kind + '/0001')
            self.assertEqual(result['expression_failures'], 0)
            for index, state, position in expected:
                with self.subTest(kind=kind, state=state):
                    frame = result['frames'][index]
                    live = frame['controllers']['states'][kind + '.main']
                    self.assertEqual(live['state'], state)
                    self.assertEqual([clip['name'] for clip in live['animations'] if clip['applied']],
                                     [f'wx_{kind}_{state}'])
                    self.assertEqual(frame['pose']['Base']['position'], position)
