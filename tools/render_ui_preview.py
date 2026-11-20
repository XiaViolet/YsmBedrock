"""Render the real C++ UI draw list for art review; this is not an in-game capture."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import zipfile
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / 'build/windows/x64/release/ysm-inspect.exe'


def call(*args):
    result = subprocess.run([str(EXE), *map(str, args)], capture_output=True, encoding='utf-8', timeout=40)
    if result.returncode:
        raise RuntimeError(result.stderr)
    return json.loads(result.stdout)


def source_json(path, name):
    if path.is_file():
        with zipfile.ZipFile(path) as archive:
            manifest = next(n for n in archive.namelist() if n == 'ysm.json' or n.endswith('/ysm.json'))
            return json.loads(archive.read(manifest[:-8] + name).decode('utf-8-sig'))
    root = path if (path / 'ysm.json').exists() else next(path.rglob('ysm.json')).parent
    return json.loads((root / name).read_text(encoding='utf-8-sig'))


def render_frame(frame, output, model_image, width, height):
    supersample = 2
    canvas = Image.new('RGBA', (width * supersample, height * supersample), (126, 146, 121, 255))
    fonts = {}
    font_path = Path('C:/Windows/Fonts/msyh.ttc')

    def paint(item):
        color = item['color']
        rgba = ((color >> 16) & 255, (color >> 8) & 255, color & 255, (color >> 24) & 255)
        target = canvas if rgba[3] == 255 else Image.new('RGBA', canvas.size)
        draw = ImageDraw.Draw(target)
        x, y, w, h = [item[k] * supersample for k in ('x', 'y', 'w', 'h')]
        if 'polygon' in item:
            draw.polygon([(px*supersample,py*supersample) for px,py in item['polygon']], fill=rgba)
        elif not item['text']:
            draw.rounded_rectangle((x, y, x + w, y + h), radius=item['radius'] * supersample, fill=rgba)
        else:
            size = max(1, round(item['scale'] * 10 * supersample))
            if size not in fonts:
                fonts[size] = ImageFont.truetype(str(font_path), size)
            font = fonts[size]
            if item['centered']:
                box = draw.textbbox((0, 0), item['text'], font=font)
                x += (w - box[2] + box[0]) / 2
            draw.text((x, y), item['text'], font=font, fill=rgba, anchor='lt')
        if target is not canvas:
            canvas.alpha_composite(target)

    split = min(frame['overlay_start'], len(frame['paint']))
    for item in frame['paint'][:split]:
        if not item['text']:paint(item)
    # Use the production GDI antialiasing atlas instead of substituting a
    # Pillow font. This is still a draw-list review, not a game screenshot.
    with tempfile.TemporaryDirectory() as temp:
        temp=Path(temp);request=temp/'labels.json';raw=temp/'labels.rgba'
        request.write_text(json.dumps(dict(density=supersample,labels=[p for p in frame['paint'][:split] if p['text']]),ensure_ascii=False),encoding='utf-8')
        atlas_info=call('--ui-text-atlas',request,raw)
        atlas=Image.frombytes('RGBA',(atlas_info['width'],atlas_info['height']),raw.read_bytes())
        for q in atlas_info['quads']:
            u,v,uw,vh=q['uv'];x=round(u*atlas.width);y=round(v*atlas.height);w=round(uw*atlas.width);h=round(vh*atlas.height)
            canvas.alpha_composite(atlas.crop((x,y,x+w,y+h)),(round(q['x']*supersample),round(q['y']*supersample)))
    if model_image is not None and frame.get('preview'):
        rect = frame['preview']
        side = min(rect['w'], rect['h'])
        image = model_image.resize((round(side * supersample), round(side * supersample)), Image.Resampling.NEAREST)
        canvas.alpha_composite(image, (round((rect['x'] + (rect['w'] - side) / 2) * supersample),
                                       round((rect['y'] + (rect['h'] - side) / 2) * supersample)))
    for item in frame['paint'][split:]:
        paint(item)
    canvas.convert('RGB').resize((width, height), Image.Resampling.LANCZOS).save(output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--models', type=Path, required=True)
    parser.add_argument('--selection', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temp:
        temp = Path(temp)
        selected = json.loads(args.selection.read_text(encoding='utf-8-sig'))
        temporary_selection = temp / 'selection.json'
        temporary_selection.write_text(json.dumps(selected, ensure_ascii=False), encoding='utf-8')
        library = call('--model-library', args.models, temporary_selection, json.dumps({'actions': ['initial']}))['frames'][0]
        source = args.models / library['library']['selected_source']
        descriptor = source_json(source, 'ysm.json')
        clips = []
        for group, file in descriptor['files']['player']['animation'].items():
            if group not in ('arm', 'fp_arm'):
                clips.extend(source_json(source, file).get('animations', {}))
        width, height = 1480, 880
        data = dict(width=width, height=height, generation=1, source=source.name, paint=True,
                    descriptor=descriptor, catalog=library['library']['catalog'],
                    textures=[t['name'] for t in library['model']['textures']],
                    clips=clips)
        rgba = temp / 'model.rgba'
        dimensions = call('--model-preview', source, rgba, json.dumps({'texture': library['model']['selected_texture']}))
        image = Image.frombytes('RGBA', (dimensions['width'], dimensions['height']), rgba.read_bytes())
        image.save(args.output / 'model.png')
        for page in ('models', 'preview', 'settings', 'roulette'):
            events = [dict(op='open', wheel=page == 'roulette')]
            if page in ('preview', 'settings'):
                base = call('--ui-session', json.dumps(dict(data, events=events), ensure_ascii=False))[-1]
                label = '\u52a8\u4f5c\u9884\u89c8' if page == 'preview' else '\u672c\u5730\u8bbe\u7f6e'
                target = next(t for t in base['text'] if t['text'] == label)
                events.append(dict(op='mouse', button=1, value=1, x=target['x']+5, y=target['y']+2))
            frame = call('--ui-session', json.dumps(dict(data, events=events), ensure_ascii=False))[-1]
            (args.output / (page + '.json')).write_text(json.dumps(frame, ensure_ascii=False), encoding='utf-8')
            render_frame(frame, args.output / (page + '.png'), image, width, height)
        print(json.dumps({'output': str(args.output), 'pages': 4, 'type': 'C++ draw-list preview, not runtime capture'}))


if __name__ == '__main__':
    main()
