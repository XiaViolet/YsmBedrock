"""Generate animation tag lookups from the pinned Java/YSM data resources."""
import collections
import hashlib
import json
import pathlib
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
REFERENCE = ROOT / 'vendor/minecraft-1.20.1'


def generate():
    definitions = {'items': {}, 'entity_types': {}, 'blocks': {}, 'worldgen/biome': {}}
    sources = []

    def add(name, data):
        parts = pathlib.PurePosixPath(name).parts
        if len(parts) < 5 or parts[0] != 'data' or parts[2] != 'tags':
            return
        start = 5 if parts[3:5] == ('worldgen', 'biome') else 4
        kind = '/'.join(parts[3:start])
        if kind not in definitions:
            return
        tag = parts[1] + ':' + '/'.join(parts[start:])[:-5]
        document = json.loads(data)
        values = definitions[kind].setdefault(tag, [])
        if document.get('replace', False):
            values.clear()
        values.extend(document['values'])

    for name in ['client.jar', 'forge-47.4.3-universal.jar']:
        path = REFERENCE / name
        digest = hashlib.sha1(path.read_bytes()).hexdigest()
        if name == 'client.jar':
            assert digest == '0c3ec587af28e5a785c0b4a7b8a30f9a8f78f838'
        else:
            assert digest == (REFERENCE / (name + '.sha1')).read_text(encoding='utf-8-sig').strip()
        sources.append({'file': name, 'sha1': digest})
        with zipfile.ZipFile(path) as archive:
            for member in archive.namelist():
                if member.endswith('.json') and '/tags/' in member:
                    add(member, archive.read(member))
    data_root = ROOT / 'vendor/ysm-upstream/src/main/resources'
    for path in sorted((data_root / 'data/ysm/tags').rglob('*.json')):
        add(path.relative_to(data_root).as_posix(), path.read_bytes())

    tables, sizes = {}, {}
    for kind, tags in definitions.items():
        resolved, missing = {}, set()

        def resolve(tag, visiting=()):
            if tag in resolved:
                return resolved[tag]
            if tag in visiting:
                raise ValueError('Tag cycle: ' + repr(visiting + (tag,)))
            members = set()
            for entry in tags[tag]:
                selector = entry if isinstance(entry, str) else entry['id']
                required = isinstance(entry, str) or entry.get('required', True)
                if selector.startswith('#'):
                    target = selector[1:]
                    if ':' not in target:
                        target = 'minecraft:' + target
                    if target in tags:
                        members.update(resolve(target, visiting + (tag,)))
                    elif required:
                        missing.add(target)
                else:
                    if ':' not in selector:
                        selector = 'minecraft:' + selector
                    # Other Java mods are deliberately outside this vanilla
                    # host snapshot. Explicit native tags remain available.
                    if selector.startswith('minecraft:'):
                        members.add(selector)
            resolved[tag] = members
            return members

        by_id = collections.defaultdict(list)
        for tag in sorted(tags):
            for identifier in sorted(resolve(tag)):
                by_id[identifier].append(tag)
        if missing:
            raise ValueError('Unresolved required ' + kind + ' tags: ' + repr(sorted(missing)))
        tables[kind] = dict(sorted(by_id.items()))
        sizes[kind] = {'tag_count': len(tags), 'ids': len(by_id), 'memberships': sum(map(len, by_id.values()))}

    lines = ['// Generated from pinned Java 1.20.1, Forge 47.4.3 and YSM data.',
             '// Regenerate with tools/generate_java_tags.py; see licenses/java-animation-inputs.md.']
    for kind, macro in [('items', 'YSM_JAVA_ITEM_TAGS'), ('entity_types', 'YSM_JAVA_ENTITY_TAGS'),
                        ('blocks', 'YSM_JAVA_BLOCK_TAGS'), ('worldgen/biome', 'YSM_JAVA_BIOME_TAGS')]:
        lines.append('#ifdef ' + macro)
        for identifier, tags in tables[kind].items():
            lines.append(macro + '(' + json.dumps(identifier) + ', ' + json.dumps(' '.join(tags)) + ')')
        lines.append('#endif')
    (ROOT / 'src/JavaVanillaTags.inc').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    record = {'minecraft': '1.20.1', 'forge': '47.4.3', 'ysm': '15c2c25f5b50f54ee099bc474483dc4b588d956d',
              'sources': sources, 'sizes': sizes}
    (REFERENCE / 'resolved-tags.json').write_text(json.dumps(tables, indent=2), encoding='utf-8')
    (ROOT / 'licenses/java-animation-tags-source.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
    print(json.dumps(record))


if __name__ == '__main__':
    generate()
