"""Exercise the same in-game UI state, authored routes and selection backend."""
import json
import hashlib
import pathlib
import struct
import subprocess
import tempfile
import unittest
import zipfile
import zlib
from test_import import EXE, SAMPLE
import test_model_library as library_tests
from test_model_textures import png


def run(mode, value):
    result = subprocess.run([str(EXE), mode, json.dumps(value, ensure_ascii=False)],
                            capture_output=True, encoding='utf-8', timeout=20)
    if result.returncode:
        raise AssertionError(result.stderr)
    return json.loads(result.stdout)


class UiTests(unittest.TestCase):
    def test_wearer_form_snapshot_restores_face_scalars_without_rerunning_labels(self):
        forms = [dict(type="radio", value="v.pants_change8", labels={"smile": "v.pants_change8=4;v.t8=6;v.roaming.other=9"}),
                 dict(type="checkbox", value="v.sld")]
        description = dict(properties=dict(extra_animation_buttons=[dict(id="face",config_forms=forms)]))
        owner = run("--ui-bindings",dict(descriptor=description,events=[
            dict(op="action",route="face/0",label=0),dict(op="action",route="face/1",value=1)]))[-1]
        self.assertEqual(owner["form_snapshot"],{"variable.pants_change8":4,"variable.t8":6,"variable.sld":1})
        snapshot = dict(owner["form_snapshot"], **{"variable.unrelated":123,"variable.roaming.other":123})
        observer = run("--ui-bindings",dict(descriptor=description,events=[dict(form_snapshot=snapshot)]))[0]
        self.assertEqual(observer["form_snapshot"],owner["form_snapshot"])
        self.assertEqual(observer["values"],owner["values"])
        self.assertEqual(observer["roaming"],{})

    def session(self, events, **data):
        data.setdefault('source', 'model.zip')
        return run('--ui-session', dict(generation=1, events=events, **data))

    def click(self, events, label, **data):
        frame = self.session(events, **data)[-1]
        target = next(t for t in frame['text'] if t['text'] == label)
        return events + [dict(op='mouse', button=1, value=1,
                              x=target['x']+target['w']/2, y=target['y']+2)]

    def test_author_language_applies_to_open_catalog_wheel_and_form(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            pack = root / 'language.zip'
            geometry = {'minecraft:geometry': [{'description': {'texture_width': 2, 'texture_height': 2},
                                                'bones': [{'name': 'root'}]}]}
            form_key = 'properties.extra_animation_buttons.appearance.config_forms.0'
            manifest = dict(spec=2, metadata=dict(name='Base model', tips='Base tip', authors=[dict(name='Base author')]),
                            properties=dict(extra_animation={'config': '#appearance'}, extra_animation_buttons=[dict(
                                id='appearance', name='Base settings', config_forms=[dict(type='radio', title='Base title',
                                    description='Base description', value='v.choice', labels={'Base option': 'v.choice=0;'})])]),
                            files=dict(player=dict(model=dict(main='main.json', arm='arm.json'), texture=['skin.png']),
                                       language_path='/local/words/'))
            english = {'metadata.name': 'English model', 'metadata.tips': 'English tip',
                       'metadata.authors.0.name': 'English author', 'properties.extra_animation.config': 'English wheel',
                       'files.player.texture.skin': 'English skin', f'{form_key}.title': 'English title',
                       f'{form_key}.description': 'English description', f'{form_key}.labels.0': 'English option'}
            french = {'metadata.name': 'French model', 'metadata.tips': 42,
                      'properties.extra_animation.config': 'French wheel', 'properties.extra_animation.config.desc': 'French note',
                      'files.player.texture.skin': 'French skin', f'{form_key}.title': 'French title',
                      f'{form_key}.labels.0': 'French option'}
            with zipfile.ZipFile(pack, 'w') as archive:
                for name, document in {'ysm.json': manifest, 'main.json': geometry, 'arm.json': geometry,
                        'local/words/en_us.json': english, 'local/words/fr_fr.JSON': french,
                        'local/words/pt_br.json': {}, 'local/words/nested/fr_fr.json': {'metadata.name': 'Wrong nested locale'},
                        'lang/zh_cn.json': {'metadata.name': 'Wrong default folder'}}.items():
                    archive.writestr(f'wrapper/{name}', json.dumps(document))
                archive.writestr('wrapper/skin.png', library_tests.png())
            built = subprocess.run([str(EXE), '--model-library', str(root), str(root / 'selection.json'),
                                    json.dumps({'actions': ['initial']})], capture_output=True, encoding='utf-8', timeout=20, check=True)
            catalog = json.loads(built.stdout)['frames'][0]['attempt']['catalog']
            self.assertEqual(set(catalog[0]['languages']), {'en_us', 'fr_fr', 'pt_br', 'nested/fr_fr'})
            data = dict(pack=str(pack), locale='fr-FR', width=960, height=540, catalog=catalog)
            def labels(frame):
                return {line['text'] for line in frame['text']}
            open_menu = [dict(op='open')]
            frames = self.session(open_menu + [dict(op='update', locale='zh_CN'), dict(op='update', locale='pt_BR')], **data)
            self.assertTrue({'French model', 'Base author', 'Base tip'} <= labels(frames[0]))
            self.assertTrue({'English model', 'English author', 'English tip'} <= labels(frames[1]))
            self.assertTrue({'Base model', 'Base author', 'Base tip'} <= labels(frames[2]))
            search = self.click(open_menu, '搜索名称（可粘贴中文）', **data)
            query = dict(op='key', key=86, control=True, text='French model')
            filtered = self.session(search + [query, dict(op='update', locale='en_US')], **data)
            self.assertIn('1 个模型', labels(filtered[-2]))
            self.assertIn('0 个模型', labels(filtered[-1]))
            textures = self.click(open_menu, '贴图', **data)
            self.assertIn('French skin', labels(self.session(textures, **data)[-1]))
            wheel = [dict(op='open', wheel=True)]
            target=next(t for t in self.session(wheel, **data)[-1]['text'] if t['text']=='French wheel')
            wheel.append(dict(op='mouse', x=target['x']+target['w']/2, y=target['y']+2))
            self.assertTrue({'French wheel', 'French note'} <= labels(self.session(wheel, **data)[-1]))
            forms = wheel + [dict(op='key', key=49)]
            frames = self.session(forms + [dict(op='update', locale='en_US')], **data)
            self.assertTrue({'French title', 'Base description', 'French option'} <= labels(frames[-2]))
            self.assertTrue({'English title', 'English description', 'English option'} <= labels(frames[-1]))
            chosen = self.click(forms, 'French option', **data)
            self.assertEqual(self.session(chosen, **data)[-1]['commands'],
                             [dict(kind='form', key='appearance/0', value=0, label=0, generation=1)])

    def test_original_shortcuts_have_edge_detection(self):
        events = [dict(key=18, down=True), dict(key=89, down=True), dict(key=89, down=True),
                  dict(key=89, down=False), dict(key=18, down=False), dict(key=90, down=True)]
        self.assertEqual(run('--input-events', events), ['none', 'model_menu', 'none', 'none', 'none', 'roulette'])

    def test_preview_drag_pan_zoom_and_reset_stay_inside_model_menu(self):
        data = dict(paint=True, width=960, height=540)
        events = self.click([dict(op='open')], '动作预览', **data)
        rect = self.session(events, **data)[-1]['preview']
        x, y = rect['x'] + rect['w']/2, rect['y'] + rect['h']/2
        def mouse(button, value, dx=0, dy=0):
            return dict(op='mouse', button=button, value=value, x=x+dx, y=y+dy)
        events += [mouse(1, 1), mouse(0, 0, 30, -20), mouse(1, 0, 30, -20),
                   mouse(2, 1), mouse(0, 0, 20, 20), mouse(2, 0, 20, 20), mouse(4, 1)]
        before = self.session(events, **data)[-1]
        self.assertAlmostEqual(before['preview_camera'][0], 60, delta=.0001)
        self.assertAlmostEqual(before['preview_camera'][1], 13, delta=.0001)
        self.assertGreater(before['preview_camera'][2], 1)
        self.assertTrue(all(v > 0 for v in before['preview_camera'][3:]))
        self.assertEqual(before['commands'], [])
        after = self.session(self.click(events, '复位预览', **data), **data)[-1]
        self.assertEqual(after['preview_camera'], [15, -7, 1, 0, 0])

    def test_wheel_category_return_and_play_use_declared_order(self):
        descriptor = {'properties': {'extra_animation': {'#dance': '舞蹈'},
                      'extra_animation_classify': [{'id': 'dance', 'extra_animation':
                                                  {'hello': '招手', '#return': '返回'}}]}}
        frames = self.session([dict(op='open', wheel=True), dict(op='key', key=49),
                               dict(op='key', key=50), dict(op='key', key=49), dict(op='key', key=49)],
                              descriptor=descriptor)
        self.assertEqual([f['category'] for f in frames[:4]], ['', 'dance', '', 'dance'])
        self.assertFalse(frames[-1]['open'])
        self.assertEqual(frames[-1]['commands'], [dict(kind='extra', key='hello', value=0, label=-1, generation=1)])

    def test_live_decoration_keeps_wheel_and_play_closes_it(self):
        data=dict(enabled=True, paint=True, clips=['both','dance'],
                  browsing={'source':'unworn.zip','name':'Unworn','clips':[]},
                  descriptor={'properties':{'extra_animation':{'both':'#hat','dance':'Dance'},
                  'extra_animation_buttons':[{'id':'hat','name':'Hat','config_forms':[
                      {'type':'checkbox','title':'Ribbon','value':'v.roaming.ribbon'}]}]}})
        wheel=[dict(op='open',wheel=True)]
        form=self.click(wheel,'设置',**data)
        change=self.click(form,'已关闭',**data)
        frames=self.session(change+[dict(op='key',key=50)],**data)
        for frame in frames[:-1]:
            self.assertTrue(frame['live_player_preview'] and frame['open'])
            self.assertFalse(frame['candidate_preview'])
            shapes=[p['polygon'] for p in frame['paint'] if 'polygon' in p]
            self.assertEqual(len(shapes),8)
            self.assertLess(frame['preview']['x']+frame['preview']['w'],min(x for pts in shapes for x,y in pts))
        self.assertEqual([c['key'] for c in frames[-2]['commands']],['hat/0'])
        self.assertFalse(frames[-1]['open'])
        self.assertEqual([c['key'] for c in frames[-1]['commands']],['dance'])
        self.assertEqual(self.session(wheel+[dict(op='key',key=49)],**data)[-1]['commands'][0]['key'],'both')
        # The thin gap between neighbouring painted sectors has no click target.
        a,b=shapes[0],shapes[1]
        mid=len(a)//2
        gap=[sum(p[i] for p in [a[mid-1],a[mid],b[0],b[-1]])/4 for i in (0,1)]
        blank=self.session(wheel+[dict(op='mouse',button=1,value=1,x=gap[0],y=gap[1])],**data)[-1]
        self.assertTrue(blank['open'])
        self.assertEqual(blank['commands'],[])

    def test_wheel_portrait_is_larger_and_clear_of_clickable_sectors(self):
        for width,height,hotbar in ((640,360,330),(480,640,610),(1280,480,455)):
            frame=self.session([dict(op='open',wheel=True)],enabled=True,paint=True,width=width,height=height,hotbar_top=hotbar)[-1]
            rect=frame['preview']
            scale=min(width/952,min(hotbar,height-24)/568)
            self.assertGreaterEqual(min(rect['w'],rect['h']),250*scale)
            self.assertGreaterEqual(rect['x'],0)
            self.assertLess(rect['x']+rect['w'],min(x for p in frame['paint'] if 'polygon' in p for x,y in p['polygon']))
            self.assertLess(rect['y']+rect['h'],hotbar)

    def test_portrait_rate_is_adjustable_with_hud_hidden(self):
        data=dict(width=920,height=540,paint=True)
        settings=self.click([dict(op='open')],'本地设置',**data)
        frame=self.session(settings,**data)[-1]
        hint=next(t for t in frame['text'] if t['text']=='纸娃娃帧率  30')
        track=next(p for p in frame['paint'] if p['color']==0xffe0e7df and p['w']>100
                   and p['x']>hint['x'] and abs(p['y']-hint['y'])<12)
        for fraction,rate in ((0,5),(.4,15),(.999,30)):
            clicked=dict(op='mouse',button=1,value=1,x=track['x']+fraction*track['w'],y=track['y']+1)
            self.assertEqual(self.session(settings+[clicked],**data)[-1]['commands'][0],
                             dict(kind='setting',key='hud_fps',value=rate,label=-1,generation=1))

    def test_portrait_cadence_30_fps_and_no_catchup_burst(self):
        for game_rate in (60,120,144):
            times=[i/game_rate for i in range(game_rate*2)]
            for fps in (5,15,30,100):
                result=run('--portrait-cadence',dict(times=times,fps=fps))
                self.assertEqual(len(result),2*min(fps,30))
        self.assertEqual(run('--portrait-cadence',dict(times=[0,.001,2,2.001,2.033,2.034])),[0,2,2.034])

    def test_ninth_wheel_entry_and_empty_slots(self):
        descriptor = {'properties': {'extra_animation': {f'dance{i}': str(i) for i in range(9)}}}
        frames = self.session([dict(op='open', wheel=True), dict(op='key', key=34),
                               dict(op='key', key=56), dict(op='key', key=49)], descriptor=descriptor)
        self.assertEqual(frames[1]['wheel_page'], 1)
        self.assertTrue(frames[2]['open'])
        self.assertEqual(frames[-1]['commands'][0]['key'], 'dance8')

    def test_generation_change_clears_old_category(self):
        descriptor = {'properties': {'extra_animation': {'#x': 'x'},
                      'extra_animation_classify': [{'id': 'x', 'extra_animation': {'a': 'A'}}]}}
        frames = self.session([dict(op='open', wheel=True), dict(op='key', key=49),
                               dict(op='update', generation=2), dict(op='key', key=49)], descriptor=descriptor)
        self.assertEqual(frames[2]['category'], '')
        self.assertEqual(frames[3]['category'], 'x')

    def test_chinese_search_can_be_pasted_and_backspace_removes_one_character(self):
        data = dict(catalog=[dict(source='绘梨衣.zip'), dict(source='大肥鱼.zip')])
        events = self.click([dict(op='open')], '搜索名称（可粘贴中文）', **data)
        events += [dict(op='key', key=86, control=True, text='绘梨衣'), dict(op='key', key=8)]
        frames = self.session(events, **data)
        self.assertEqual(frames[-1]['search'], '绘梨')
        labels = [t['text'] for t in frames[-1]['text']]
        self.assertIn('绘梨衣', labels)
        self.assertNotIn('大肥鱼', labels)

    def test_hovered_model_name_stays_on_card_and_out_of_import_footer(self):
        data = dict(catalog=[dict(source='黄鱼.ysm', name='黄色大肥鱼')])
        opened = [dict(op='open')]
        before = self.session(opened, **data)[-1]
        card = next(t for t in before['text'] if t['text'] == '黄色大肥鱼')
        hover = dict(op='mouse', x=card['x'] + 4, y=card['y'] + 2)
        after = self.session(opened + [hover], **data)[-1]
        self.assertEqual([t for t in after['text'] if t['text'] == '黄色大肥鱼'], [card])
        self.assertIn('导入模型', {t['text'] for t in after['text']})

    def test_catalog_click_addresses_that_exact_source(self):
        data = dict(catalog=[dict(source='same', name='目录'), dict(source='same.zip', name='压缩包')])
        events = self.click([dict(op='open')], '压缩包', **data)
        command, = self.session(events, **data)[-1]['commands']
        self.assertEqual((command['kind'], command['key']), ('browse', 'same.zip'))

    def test_candidate_has_private_actions_and_only_wear_selects_for_world(self):
        data = dict(enabled=True, clips=['worn_only'], browsing=dict(source='候选.zip', name='候选',
                    animation='greet', clips=['greet', 'dance']), width=960, height=540, paint=True)
        opened = [dict(op='open')]
        frame = self.session(opened, **data)[-1]
        self.assertTrue(frame['candidate_preview'] and frame['animate_preview'])
        self.assertEqual(frame['preview_clip'], 'greet')
        self.assertEqual(frame['source'], 'model.zip')
        worn = self.session(self.click(opened, '穿戴此模型', **data), **data)[-1]
        self.assertEqual([(c['kind'], c['key']) for c in worn['commands']], [('wear', '候选.zip')])
        actions = self.click(opened, '动作预览', **data)
        selected = self.click(actions, 'dance', **data)
        frame = self.session(selected, **data)[-1]
        self.assertEqual(frame['preview_clip'], 'dance')
        self.assertEqual(frame['commands'], [])
        self.assertNotIn('worn_only', {t['text'] for t in frame['text']})
        auto = self.session(self.click(selected, '自动动作', **data), **data)[-1]
        self.assertEqual((auto['commands'], auto['preview_clip']), ([], 'greet'))
        unrelated = self.session(self.click(selected, '贴图', **data), **data)[-1]
        self.assertFalse(unrelated['candidate_preview'])
        self.assertEqual(unrelated['preview_clip'], '')
        loading = self.session(opened + [dict(op='update', browse_loading='another.zip')], **data)[-1]
        self.assertNotIn('preview', loading)
        self.assertNotIn('穿戴此模型', {t['text'] for t in loading['text']})

    def test_import_actions_are_available_in_an_empty_catalog(self):
        data = dict(source='', enabled=False, catalog=[])
        opened = [dict(op='open')]
        picker = self.click(opened, '导入模型', **data)
        for text, kind in [('选择模型包', 'import_file'), ('选择源文件夹', 'import_folder'), ('打开模型文件夹', 'open_models_folder')]:
            frame = self.session(self.click(picker, text, **data), **data)[-1]
            self.assertEqual([c['kind'] for c in frame['commands']], [kind])
            if kind == 'open_models_folder':
                self.assertIn('选择模型包', {t['text'] for t in frame['text']})
        # Native scale 1, refresh icon centered at (613, 126).
        frame = self.session(opened + [dict(op='mouse', button=1, value=1, x=613, y=126)],
                             width=952, height=592, **data)[-1]
        self.assertEqual([(c['kind'], c['key']) for c in frame['commands']], [('reload', '')])

    def test_footer_clears_the_live_hotbar_across_gui_scales(self):
        for w,h,top in [(512,288,264),(320,180,149),(960,540,501),(480,640,-1)]:
            args = dict(width=w,height=h,hotbar_top=top)
            frame = self.session(self.click([dict(op='open')],'贴图',**args),**args)[-1]
            keys = [t for t in frame['text'] if t['text'] in {'Tab','Enter','Esc','选择','返回'}]
            self.assertEqual(len(keys),5)
            for key in keys:
                self.assertLess(key['y']+key['h'], min(h-24,top) if top>0 else h-24)

    def test_native_menu_preview_can_be_disabled_without_wearing_a_model(self):
        events = self.click([dict(op='open')], '本地设置')
        frame = self.session(self.click(events, '原生网格预览'))[-1]
        self.assertEqual([(c['kind'], c['key'], c['value']) for c in frame['commands']],
                         [('setting', 'native_menu_preview', 0)])

    def test_arke_extra_layer_is_opt_in_without_wearing_a_model(self):
        events = self.click([dict(op='open')], '本地设置')
        frame = self.session(self.click(events, 'Arke 附加发光层'))[-1]
        self.assertEqual([(c['kind'], c['key'], c['value']) for c in frame['commands']],
                         [('setting', 'arke_extra_emissive_layer', 1)])

    def test_native_menu_mesh_moves_with_the_authored_walk(self):
        options = json.dumps(dict(clip='walk', times=[.12, .45]))
        result = subprocess.run([str(EXE), '--preview-mesh', str(SAMPLE), options],
                                capture_output=True, encoding='utf-8', check=True, timeout=30)
        frames = json.loads(result.stdout)
        self.assertTrue(all(f['valid'] for f in frames))
        self.assertNotEqual(frames[0]['sha256'], frames[1]['sha256'])

    def test_form_only_button_does_not_require_a_fake_animation_clip(self):
        data = dict(descriptor={'properties': {'extra_animation': {'extra': '#appearance'},
                    'extra_animation_buttons': [{'id': 'appearance', 'name': '外观', 'config_forms':
                    [{'type': 'checkbox', 'title': '帽子', 'value': 'v.roaming.hat'}]}]}})
        events = [dict(op='open', wheel=True), dict(op='key', key=49)]
        events = self.click(events, '已关闭', **data)
        frame = self.session(events, **data)[-1]
        self.assertEqual(frame['commands'], [dict(kind='form', key='appearance/0', value=1, label=-1, generation=1)])

    def test_open_model_menu_locks_movement_but_unlocked_wheel_does_not(self):
        self.assertTrue(self.session([dict(op='open')])[-1]['blocks_movement'])
        self.assertFalse(self.session([dict(op='open', wheel=True)])[-1]['blocks_movement'])

    def test_action_lock_changes_playback_setting_without_freezing_wheel_navigation(self):
        start=[dict(op='open',wheel=True)]
        events=self.click(start,'动作锁：关')
        frame=self.session(events)[-1]
        self.assertTrue(frame['open'])
        self.assertFalse(frame['blocks_movement'])
        self.assertEqual([(c['key'],c['value']) for c in frame['commands']],[('movement_lock',1)])
        unlocked=self.session(self.click(events,'动作锁：开'))[-1]
        self.assertEqual(unlocked['commands'][0]['value'],0)

    def test_config_routes_write_declared_roaming_and_quantize_ranges(self):
        descriptor = {'properties': {'extra_animation_buttons': [{'id': 'appearance', 'config_forms': [
            dict(type='checkbox', value='v.roaming.hat'),
            dict(type='range', value='v.roaming.size', min=.1, max=2, step=.1),
            dict(type='radio', value='v.face', labels={'正常': 'v.face=0;', '笑': 'v.face=1;'})]}]}}
        frames = run('--ui-bindings', dict(descriptor=descriptor, events=[dict(op='observe'),
            dict(op='action', route='appearance/0', value=1),
            dict(op='action', route='appearance/1', value=8),
            dict(op='action', route='appearance/2', label=1),
            dict(op='action', route='appearance/9', value=1)]))
        self.assertEqual(frames[1]['values']['appearance/0'], 1)
        self.assertEqual(frames[2]['values']['appearance/1'], 2)
        self.assertEqual(frames[3]['values']['appearance/2'], 1)
        self.assertEqual(frames[3]['roaming'], {'hat': 1.0, 'size': 2.0})
        self.assertFalse(frames[4]['changed'])
        self.assertEqual(frames[-1]['failures'], 0)

    def test_action_only_radio_runs_its_authored_label(self):
        descriptor = {'properties': {'extra_animation_buttons': [{'id': 'commands', 'config_forms': [
            dict(type='radio', value='', labels={'A': 'v.roaming.counter=v.roaming.counter+1;'})]}]}}
        frames = run('--ui-bindings', dict(descriptor=descriptor, events=[dict(op='action', route='commands/0', label=0)]))
        self.assertEqual(frames[0]['roaming']['counter'], 1)

    def test_observation_cannot_increment_live_variables(self):
        descriptor = {'properties': {'extra_animation_buttons': [{'id': 'commands', 'config_forms': [
            dict(type='radio', value='v.n', labels={'A': 'v.n=v.n+1;'})]}]}}
        frames = run('--ui-bindings', dict(descriptor=descriptor, events=[dict(op='action', route='commands/0', label=0),
                                                                 dict(op='observe'), dict(op='observe')]))
        self.assertEqual([f['values']['commands/0'] for f in frames], [1, 1, 1])

    def test_continuous_and_fixed_author_ranges_are_valid(self):
        descriptor = {'properties': {'extra_animation_buttons': [{'id': 'face', 'config_forms': [
            dict(type='range', value='v.roaming.scale', min=0, max=1, step=0),
            dict(type='range', value='v.roaming.fixed', min=0, max=0, step=0)]}]}}
        frames = run('--ui-bindings', dict(descriptor=descriptor, events=[
            dict(op='action', route='face/0', value=.375), dict(op='action', route='face/1', value=20)]))
        self.assertEqual(frames[0]['values']['face/0'], .375)
        self.assertEqual(frames[1]['values']['face/1'], 0)
        self.assertEqual(frames[-1]['failures'], 0)

    def test_keyboard_can_change_pages_and_exit(self):
        frames = self.session([dict(op='open'), dict(op='key', key=9), dict(op='key', key=9),
                               dict(op='key', key=13), dict(op='key', key=27), dict(op='key', key=27)])
        self.assertEqual(frames[3]['page'], 2)
        self.assertEqual(frames[4]['page'], 1)
        self.assertFalse(frames[5]['open'])

    def test_switching_pages_ends_search_input(self):
        events = self.click([dict(op='open')], '搜索名称（可粘贴中文）')
        events = self.click(events, '本地设置')
        frame = self.session(events + [dict(op='key', key=65, text='a')])[-1]
        self.assertEqual(frame['search'], '')

    def test_keyboard_can_leave_settings_switch_with_tab(self):
        events = self.click([dict(op='open')], '本地设置')
        events = self.click(events, '使用原版外观')
        frame = self.session(events + [dict(op='key', key=9), dict(op='key', key=13)])[-1]
        self.assertEqual(frame['commands'][0]['key'], 'disable_self_hands')

    def test_button_without_coordinates_uses_last_window_motion_at_all_gui_scales(self):
        for gui_w, gui_h, pixel_w, pixel_h in [(640,360,2560,1440),(320,180,2560,1440),(960,540,1920,1080)]:
            data = dict(width=gui_w, height=gui_h)
            frame = self.session([dict(op='open')], **data)[-1]
            target = next(t for t in frame['text'] if t['text'] == '本地设置')
            px=(target['x']+target['w']/2)*pixel_w/gui_w
            py=(target['y']+2)*pixel_h/gui_h
            base=dict(op='window_mouse',client_width=pixel_w,client_height=pixel_h)
            frames=self.session([dict(op='open'),dict(base,button=0,x=px,y=py),
                                 dict(base,button=1,value=1,x=0,y=0)],**data)
            self.assertEqual(frames[-1]['page'],4)

    def test_window_cursor_overrides_unusable_button_event(self):
        frame=self.session([dict(op='open')])[-1]
        target=next(t for t in frame['text'] if t['text']=='贴图')
        frames=self.session([dict(op='open'),dict(op='window_mouse',button=1,value=1,x=0,y=0,
                            window_x=(target['x']+4)*4,window_y=(target['y']+2)*4,
                            client_width=2560,client_height=1440)])
        self.assertEqual(frames[-1]['page'],2)

    def test_dragging_continuous_slider_emits_the_authored_route(self):
        data=dict(width=920,height=540,paint=True,descriptor={'properties':{'extra_animation':{'extra':'#face'},
                  'extra_animation_buttons':[{'id':'face','name':'脸部选项','config_forms':[
                      dict(type='range',value='v.roaming.size',min=0,max=1,step=0)]}]}})
        events=[dict(op='open',wheel=True),dict(op='key',key=49)]
        frame=self.session(events,**data)[-1]
        # Select the teal track from the real paint output. This tests pointer
        # hit geometry and the route, including release, rather than a setter.
        track=next(p for p in frame['paint'] if p['text']=='' and p['color']==0xffe0e7df and p['w']>150 and 2<p['h']<5)
        events += [dict(op='mouse',button=1,value=1,x=track['x']+track['w']*.375,y=track['y']+1),
                   dict(op='mouse',button=0,x=track['x']+track['w']*.625,y=track['y']+1),
                   dict(op='mouse',button=1,value=0,x=track['x']+track['w']*.625,y=track['y']+1)]
        frames=self.session(events,**data)
        writes=[c for f in frames for c in f['commands'] if c['kind']=='form']
        self.assertEqual(writes[-1]['key'],'face/0')
        self.assertAlmostEqual(writes[-1]['value'],.625,places=5)

    def test_typography_stays_inside_each_page_at_small_gui_sizes(self):
        for width,height in [(320,180),(640,360),(960,540),(480,640)]:
            events=[dict(op='open')]
            frames=self.session(events,width=width,height=height)
            for text in frames[-1]['text']:
                self.assertGreaterEqual(text['x'],0)
                self.assertGreaterEqual(text['y'],0)
                self.assertLessEqual(text['x']+text['w'],width+.1)
                self.assertLessEqual(text['y']+text['h'],height+.1)

    def test_shapes_are_submitted_as_bounded_meshes_not_thousands_of_draws(self):
        data=dict(catalog=[dict(source='a.zip'),dict(source='b.zip')])
        for width,height in [(512,288),(1480,880),(2560,1440)]:
            for page in ['模型库','动作预览','本地设置']:
                events=[dict(op='open')]
                if page!='模型库':events=self.click(events,page,width=width,height=height,**data)
                frame=self.session(events,width=width,height=height,**data)[-1]
                self.assertGreater(frame['mesh_vertices'],100)
                self.assertLessEqual(frame['mesh_batches'],4)
                self.assertEqual(frame['mesh_vertices']%3,0)
            wheel=self.session([dict(op='open',wheel=True)],width=width,height=height)[-1]
            self.assertLessEqual(wheel['mesh_batches'],4)

    def test_multiple_native_screen_views_only_submit_ui_once_per_frame(self):
        self.assertEqual(run('--ui-frame-gate',['begin','render','render','render',
                         'begin','render','render','begin','render']),[True,False,False,True,False,True])

    def test_game_cannot_regrab_the_cursor_while_menu_remains_open(self):
        frames=run('--ui-mouse-capture',[dict(op='grab'),dict(op='open')]+
                   [dict(op='grab') for _ in range(120)]+[dict(op='close'),dict(op='grab')])
        self.assertFalse(frames[0]['blocked'])
        self.assertTrue(all(f['blocked'] for f in frames[1:-1]))
        self.assertFalse(frames[-1]['blocked'])

    def test_mouse_capture_ownership_does_not_affect_another_client(self):
        frames=run('--ui-mouse-capture',[dict(op='open',client=0),dict(op='grab',client=1),
                   dict(op='grab',client=0),dict(op='close'),dict(op='grab',client=0),dict(op='grab',client=1)])
        self.assertEqual([f['blocked'] for f in frames],[False,True,False,False])


class ExactSelectionTests(unittest.TestCase):
    setUp = library_tests.ModelLibraryTests.setUp
    source = library_tests.ModelLibraryTests.source
    preferences = library_tests.ModelLibraryTests.preferences
    run_library = library_tests.ModelLibraryTests.run_library

    def test_explicit_selection_never_falls_back_to_an_unrequested_model(self):
        self.source('a.zip'); self.source('b.zip')
        frames = self.run_library(['initial', dict(action='select', source='b.zip'),
                                  dict(action='select', source='../outside.zip')])
        self.assertEqual([f['library']['selected_source'] for f in frames], ['a.zip', 'b.zip', 'b.zip'])
        self.assertFalse(frames[-1]['attempt']['prepared'])

    def test_reload_selected_source_does_not_cycle_to_another_model(self):
        self.source('a.zip'); self.source('b.zip')
        frames = self.run_library(['initial', dict(action='select', source='a.zip')])
        self.assertEqual(frames[-1]['library']['selected_source'], 'a.zip')


class ModelPreviewTests(unittest.TestCase):
    def test_single_colored_face_matches_java_for_zero_one_two_and_three_negative_axes(self):
        fixtures = pathlib.Path(__file__).with_name('fixtures')
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / 'faces.rgba'
            for source, south in (('SignedFaceLab.zip', [True, True, False, False]),
                                  ('SignedFaceLabAllCutout.zip', [False, True, False, False])):
                self.preview(fixtures / source, output, {'yaw': 0, 'pitch': 0})
                pixels = output.read_bytes()
                for y, expected in [(74, [True, False, True, True]), (149, south)]:
                    self.assertEqual([pixels[(y * 224 + x) * 4 + 3] != 0 for x in (27, 85, 140, 196)],
                                     expected, (source, y))

    def test_zero_or_omitted_texture_dimensions_match_native_64_pixel_preview(self):
        fixture = pathlib.Path(__file__).with_name('fixtures') / 'UvLabPlain.zip'
        with tempfile.TemporaryDirectory() as directory, zipfile.ZipFile(fixture) as source:
            root = pathlib.Path(directory)
            output = root / 'view.rgba'
            self.preview(fixture, output, {'yaw': 25, 'pitch': 18})
            reference = output.read_bytes()
            colors = {reference[i:i + 4] for i in range(0, len(reference), 4)}
            self.assertGreater(len(colors), 5)
            for width, height in ((0, 64), (64, 0), (0, 0), (None, None)):
                with self.subTest(width=width, height=height):
                    geometry = json.loads(source.read('geometry.json'))
                    description = geometry['minecraft:geometry'][0]['description']
                    for key, value in (('texture_width', width), ('texture_height', height)):
                        if value is None:
                            del description[key]
                        else:
                            description[key] = value
                    model = root / 'dimensions.zip'
                    with zipfile.ZipFile(model, 'w') as target:
                        for name in source.namelist():
                            target.writestr(name, json.dumps(geometry) if name == 'geometry.json' else source.read(name))
                    self.preview(model, output, {'yaw': 25, 'pitch': 18})
                    self.assertEqual(output.read_bytes(), reference)

    def test_portrait_resources_enforce_file_and_decoded_limits_and_discard_encoded_skin(self):
        budget = 32 * 1024 * 1024
        def inspect(path):
            return subprocess.run([str(EXE), '--portrait-resource', str(path)], capture_output=True, text=True, timeout=20)
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            model = root / 'parrot.geo.json'
            model.write_bytes(b'{}' + b' ' * (budget - 2))
            self.assertTrue(json.loads(inspect(model).stdout)['loaded'])
            with model.open('ab') as out:
                out.write(b' ')
            self.assertFalse(json.loads(inspect(model).stdout)['loaded'])
            skin = root / 'parrot.png'
            skin.write_bytes(png(2, 2, (0, 255, 0, 255)))
            with skin.open('r+b') as out:
                out.truncate(budget)
            normal = inspect(skin)
            self.assertEqual(normal.returncode, 0, normal.stderr)
            self.assertEqual(json.loads(normal.stdout), {'rgba_bytes': 16, 'encoded_capacity': 0})
            with skin.open('ab') as out:
                out.write(b'x')
            large_file = inspect(skin)
            self.assertNotEqual(large_file.returncode, 0)
            self.assertIn('resource limit', large_file.stderr)
            skin.write_bytes(png(2049, 2048, (0, 255, 0, 255)))
            large_image = inspect(skin)
            self.assertNotEqual(large_image.returncode, 0)
            self.assertIn('texture', large_image.stderr.lower())

    def test_hud_perched_model_requires_occupied_exact_side_and_uses_its_own_skin(self):
        def fixture(path, bones, color):
            source = pathlib.Path(__file__).with_name('fixtures') / 'UvLabPlain.zip'
            skin = png(16, 16, color)
            with zipfile.ZipFile(source) as template, zipfile.ZipFile(path, 'w') as out:
                geometry = json.loads(template.read('geometry.json'))
                geometry['minecraft:geometry'][0]['bones'] = bones
                for name in template.namelist():
                    data = template.read(name)
                    if name == 'geometry.json':
                        data = json.dumps(geometry)
                    elif name == 'texture.png':
                        data = skin
                    out.writestr(name, data)

        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            hero, bird, frame = root / 'hero.zip', root / 'bird.zip', root / 'view.rgba'
            def cube(name, extent):
                return {'name': name, 'pivot': [0, 0, 0], 'cubes': [{'origin': [0, 0, 0], 'size': extent, 'uv': [0, 0]}]}
            fixture(hero, [cube('body', [4, 8, 3]),
                           {'name': 'RightShoulderLocator', 'parent': 'body', 'pivot': [6, 6, 0]},
                           {'name': 'LeftShoulderLocator2', 'parent': 'body', 'pivot': [-6, 6, 0]}], (255, 0, 0, 255))
            fixture(bird, [cube('bird', [2, 3, 2])], (0, 255, 0, 255))
            def pixels(**sides):
                report = self.preview(hero, frame, {'yaw': 0, 'pitch': 10, 'shoulder_pack': str(bird), **sides})
                width = report['width']
                padding = (width - 224) // 2
                red, green_count = set(), 0
                for i, (r, g, b, a) in enumerate(struct.iter_unpack('BBBB', frame.read_bytes())):
                    if r > 100 and g < 20:
                        red.add((i % width - padding, i // width - padding))
                    if g > 100 and r < 20:
                        green_count += 1
                return red, green_count
            body, _ = pixels()
            self.assertTrue(body)
            def green(**sides):
                red, green_count = pixels(**sides)
                self.assertEqual(red, body)
                return green_count
            self.assertEqual(green(), 0)
            self.assertEqual(green(left=True), 0)
            self.assertGreater(green(right=True), 150)
            self.assertEqual(green(), 0)

    def test_java_signed_shell_shows_black_core_with_default_and_cutout_material(self):
        fixtures = pathlib.Path(__file__).with_name('fixtures')
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / 'shells.rgba'
            for source in ('SignedCubeLab.zip', 'SignedCubeLabAllCutout.zip'):
                self.preview(fixtures / source, output, {'yaw': 0, 'pitch': 0})
                frame = output.read_bytes()
                for y in (74, 149):
                    samples = [frame[(y * 224 + x) * 4:(y * 224 + x) * 4 + 4]
                               for x in (27, 85, 140, 196)]
                    self.assertEqual(samples[0], bytes((0, 0, 0, 255)))
                    self.assertTrue(all(r < g < b and a == 255 for r, g, b, a in samples[1:]))

    def test_native_preview_crossing_faces_resolve_per_pixel_and_transparent_holes_show_through(self):
        fixture = pathlib.Path(__file__).with_name('fixtures') / 'MeshCoordinateLab.zip'
        def quad(name, left, right, u):
            mesh = dict(normalized_uvs=True, positions=[[-4,-4,left],[4,-4,right],[4,4,right],[-4,4,left]],
                        normals=[[0,0,1]], uvs=[[u,.5]], polys=[[[i,0,0] for i in range(4)]])
            return dict(name=name, poly_mesh=mesh)
        bones = [quad('red',-2,2,.125), quad('blue',2,-2,.375), quad('hole',-3,-3,.625)]
        colors = bytes([255,0,0,255, 0,0,255,255, 0,255,0,0, 0,255,0,128])
        def chunk(name, body):
            return struct.pack('>I',len(body))+name+body+struct.pack('>I',zlib.crc32(name+body))
        skin = (b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',4,1,8,6,0,0,0))
                +chunk(b'IDAT',zlib.compress(b'\0'+colors))+chunk(b'IEND',b''))
        with tempfile.TemporaryDirectory() as folder, zipfile.ZipFile(fixture) as src:
            root = pathlib.Path(folder)
            output = root/'test.rgba'
            def draw(surfaces, flat=False):
                model = root/'cross.zip'
                doc = json.loads(src.read('body.json'))
                doc['minecraft:geometry'][0]['bones'] = surfaces
                with zipfile.ZipFile(model,'w') as dest:
                    for name in src.namelist():
                        dest.writestr(name, json.dumps(doc) if name=='body.json' else skin if name=='atlas.png' else src.read(name))
                self.preview(model,output,dict(gpu=True,flatten_depth=flat,yaw=0,pitch=10))
                frame=output.read_bytes()
                return [tuple(frame[(112*224+x)*4:(112*224+x)*4+4]) for x in (58,166)]
            expected = [(255,0,0,255),(0,0,255,255)]
            self.assertEqual(draw(bones),expected)
            self.assertEqual(draw(list(reversed(bones))),expected)
            wrong = draw(bones,flat=True)
            self.assertEqual(wrong[0],wrong[1])
            self.assertEqual(draw(bones+[quad('glass',-4,-4,.875)]),[(127,128,0,255),(0,128,127,255)])
            self.assertEqual(draw(bones+[quad('glass',4,4,.875)]),expected)

    def test_polygon_uv_rows_match_bedrock_world_calibration(self):
        source = pathlib.Path(__file__).with_name('fixtures') / 'MeshCoordinateLab.zip'
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / 'mesh.rgba'
            self.preview(source, output, dict(yaw=0, pitch=10))
            image = output.read_bytes()
            colors = [image[(112*224+x)*4:(112*224+x)*4+4] for x in (34, 112, 190)]
            red, blue = bytes((250, 20, 15, 255)), bytes((18, 25, 248, 255))
            self.assertEqual(colors, [red, blue, red])

    def test_candidate_clip_loops_in_gui_even_with_an_authored_main_controller(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            pack = root / '角色.zip'
            geometry = {'minecraft:geometry': [{'description': {'texture_width': 2, 'texture_height': 2},
                         'bones': [{'name': 'arm', 'pivot': [0, 0, 0]}]}]}
            descriptor = {'spec': 2, 'metadata': {'name': '预览测试'}, 'files': {'player': {
                'model': {'main': 'main.json', 'arm': 'main.json'}, 'texture': ['skin.png'],
                'animation': {'main': 'anim.json'}, 'animation_controllers': ['ctrl.json']}}}
            clips = {'dance': {'animation_length': 1, 'bones': {'arm': {'position': {
                        '0.0': [0, 0, 0], '1.0': [0, 8, 0]}}}},
                     'other': {'loop': True, 'bones': {'arm': {'position': [0, 0, 0]}}}}
            controllers = {'player.main': {'initial_state': 'default', 'states': {'default': {'animations': ['other']}}}}
            with zipfile.ZipFile(pack, 'w') as archive:
                for key, value in {'ysm.json': descriptor, 'main.json': geometry,
                                   'anim.json': {'animations': clips},
                                   'ctrl.json': {'animation_controllers': controllers}}.items():
                    archive.writestr(key, json.dumps(value))
                archive.writestr('skin.png', library_tests.png())
            def offset(time):
                report = self.preview(pack, root / 'frame.rgba', dict(clip='dance', time=time, inspect=True))
                return report['evaluation']['pose']['arm']['position'][1]
            self.assertAlmostEqual(offset(.5), 4, places=4)
            self.assertAlmostEqual(offset(1.5), 4, places=4)

    def preview(self, source, output, options):
        result=subprocess.run([str(EXE),'--model-preview',str(source),str(output),json.dumps(options)],
                              capture_output=True,encoding='utf-8',timeout=20)
        self.assertEqual(result.returncode,0,result.stderr)
        return json.loads(result.stdout)

    def test_cube_uv_layout_matches_six_colored_java_menu_views(self):
        fixtures = pathlib.Path(__file__).with_name('fixtures')
        palette = {'north': (255, 10, 10), 'south': (20, 20, 255), 'east': (10, 255, 30),
                   'west': (200, 0, 200), 'up': (255, 255, 10), 'down': (5, 255, 255)}
        shades = (0.55, 0.73, 0.87, 1.0)
        views = {'north': (0, 0), 'south': (180, 0), 'west': (90, 0), 'east': (-90, 0),
                 'down': (0, 90), 'up': (0, -90)}
        swapped = {'east': 'west', 'west': 'east', 'up': 'down', 'down': 'up'}

        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / 'view.rgba'
            def draw(mode, yaw, pitch):
                self.preview(fixtures / f'UvLab{mode}.zip', output, {'yaw': yaw, 'pitch': pitch})
                return output.read_bytes()

            for mode in ('Plain', 'BoneMirror', 'CubeMirror', 'MissingSize'):
                for face, (yaw, pitch) in views.items():
                    with self.subTest(mode=mode, face=face):
                        data = draw(mode, yaw, pitch)
                        color = swapped.get(face, face) if mode.endswith('Mirror') else face
                        order = (2, 3, 0, 1) if color == 'down' else (0, 1, 2, 3)
                        if mode == 'CubeMirror':
                            order = (order[1], order[0], order[3], order[2])
                        if mode == 'MissingSize' and face == 'north':
                            order = (0, 0, 0, 0)
                        for (x, y), shade in zip(((86, 86), (138, 86), (86, 138), (138, 138)), order):
                            offset = (y * 224 + x) * 4
                            expected = bytes([*(round(c * shades[shade]) for c in palette[color]), 255])
                            self.assertEqual(data[offset:offset+4], expected)

            for face, (yaw, pitch) in views.items():
                expected_side = swapped[face] if face in ('west', 'east') else face
                with self.subTest(mode='BoxMirror', face=face):
                    # An exact side-on comparison requires compensating for the
                    # Java texture screen's ten-degree camera inclination.
                    side_yaw, side_pitch = views[expected_side]
                    correction = 10 if face in ('east', 'west') else 0
                    self.assertEqual(draw('BoxMirror', yaw, pitch + correction),
                                     draw('Box', side_yaw, side_pitch + correction))

            # The Java reference frames show the yellow top at all four
            # horizontal headings, including both sides.
            top = {tuple(round(channel * shade) for channel in palette['up']) for shade in shades}
            for yaw in (0, 90, 180, -90):
                data = draw('Plain', yaw, 0)
                count = sum(tuple(data[i:i+3]) in top for i in range(0, len(data), 4))
                self.assertGreater(count, 1000, yaw)

    def test_cube_inherits_only_its_own_bones_inflate_and_explicit_zero_wins(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / 'model'
            source.mkdir()
            with zipfile.ZipFile(pathlib.Path(__file__).with_name('fixtures') / 'WorldInputs.zip') as z:
                z.extractall(source)
            anchor = {'name': 'anchor', 'cubes': [{'origin': [12, 0, 1], 'size': [2, 3, 2], 'uv': [0, 0]}]}
            cube = {'origin': [0, 0, 0], 'size': [3, 4, 3], 'uv': [0, 0]}

            def draw(bone_values=None, cube_values=None, parent_values=None):
                bone = dict(name='body', parent='parent', cubes=[dict(cube, **(cube_values or {}))], **(bone_values or {}))
                bones = [anchor, dict(name='parent', **(parent_values or {})), bone]
                model = {'description': {'texture_width': 4, 'texture_height': 4}, 'bones': bones}
                (source / 'geometry.json').write_text(json.dumps({'minecraft:geometry': [model]}), encoding='utf-8')
                output = root / 'image.rgba'
                self.preview(source, output, dict(yaw=17, pitch=8))
                return output.read_bytes()

            base = draw()
            self.assertGreater(sum(1 for alpha in base[3::4] if alpha), 0)
            for value in (1.5, -0.5):
                with self.subTest(value=value):
                    explicit = draw(cube_values={'inflate': value})
                    self.assertNotEqual(explicit, base)
                    self.assertEqual(draw({'inflate': value}), explicit)
                    self.assertEqual(draw({'inflate': value}, {'inflate': 0}), base)
                    self.assertEqual(draw(parent_values={'inflate': value}), base)

    def test_authored_hidden_props_remain_hidden_and_source_is_readonly(self):
        before=hashlib.sha256(SAMPLE.read_bytes()).hexdigest()
        with tempfile.TemporaryDirectory() as temp:
            report=self.preview(SAMPLE,pathlib.Path(temp)/'preview.rgba',dict(inspect=True))
        self.assertEqual(report['evaluation']['expression_failures'],0)
        self.assertEqual(report['evaluation']['pose']['MiaoJian']['scale'],[0,0,0])
        self.assertEqual(report['evaluation']['pose']['gui']['scale'],[0,0,0])
        self.assertEqual(hashlib.sha256(SAMPLE.read_bytes()).hexdigest(),before)

    def test_default_preview_faces_the_model_front_instead_of_its_back(self):
        def chunk(kind,data):
            return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
        image=(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',2,1,8,6,0,0,0))
               +chunk(b'IDAT',zlib.compress(b'\0\xff\0\0\xff\0\0\xff\xff'))+chunk(b'IEND',b''))
        descriptor={'spec':2,'metadata':{'name':'Front/back test'},'files':{'player':{
            'model':{'main':'main.json','arm':'main.json'},'animation':{'main':'animation.json'},'texture':['texture.png']}}}
        geometry={'minecraft:geometry':[{'description':{'texture_width':2,'texture_height':1},'bones':[{
            'name':'root','pivot':[0,0,0],'cubes':[{'origin':[-4,0,-2],'size':[8,8,4],
            'uv':{'north':{'uv':[0,0],'uv_size':[1,1]},'south':{'uv':[1,0],'uv_size':[1,1]}}}]}]}]}
        with tempfile.TemporaryDirectory() as temp:
            temp=pathlib.Path(temp);source=temp/'model.zip'
            with zipfile.ZipFile(source,'w') as archive:
                archive.writestr('ysm.json',json.dumps(descriptor));archive.writestr('main.json',json.dumps(geometry))
                archive.writestr('animation.json','{"animations":{"idle":{"loop":true}}}');archive.writestr('texture.png',image)
            for yaw,channel in [(0,0),(180,2)]:
                output=temp/('view'+str(yaw)+'.rgba');self.preview(source,output,dict(yaw=yaw))
                pixels=output.read_bytes();offset=(112*224+112)*4
                self.assertGreater(pixels[offset+channel],150)
                self.assertEqual(pixels[offset+3],255)


class UiFontTests(unittest.TestCase):
    def test_font_is_rasterized_at_window_density_with_smooth_alpha_edges(self):
        with tempfile.TemporaryDirectory() as temp:
            temp=pathlib.Path(temp);source=temp/'text.json';output=temp/'font.rgba'
            data={'density':5,'labels':[dict(text='模型设置 Animation',x=5,y=8,w=130,h=14,scale=.537,color=0xff247968)]}
            source.write_text(json.dumps(data,ensure_ascii=False),encoding='utf-8')
            result=subprocess.run([str(EXE),'--ui-text-atlas',str(source),str(output)],capture_output=True,encoding='utf-8',timeout=15)
            self.assertEqual(result.returncode,0,result.stderr);report=json.loads(result.stdout);raw=output.read_bytes()
            self.assertEqual(report['density'],5)
            self.assertEqual(len(raw),report['width']*report['height']*4)
            self.assertEqual(len(report['quads']),1)
            self.assertGreater(len({a for a in raw[3::4] if 0<a<255}),8)
            visible=[tuple(raw[i:i+3]) for i in range(0,len(raw),4) if raw[i+3]>128]
            self.assertTrue(visible)
            self.assertTrue(all(rgb==(0x24,0x79,0x68) for rgb in visible))
