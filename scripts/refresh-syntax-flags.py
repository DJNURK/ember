#!/usr/bin/env python3
"""Regenerate scripts/syntax-check.sh from the current compile_commands.json.

Run this after changing compile definitions or include paths in CMake, so the
standalone per-file syntax check keeps matching the real build.
"""
import json, shlex, pathlib, sys

root = pathlib.Path(__file__).resolve().parent.parent
db_path = root / 'build' / 'compile_commands.json'
if not db_path.exists():
    sys.exit('configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON first')

db = json.load(open(db_path))
entries = [x for x in db if x['file'].endswith('EmberEngine.cpp')]
if not entries:
    sys.exit('could not find a reference translation unit in compile_commands.json')

parts = shlex.split(entries[0]['command'])
flags, skip = [], False
for i, p in enumerate(parts):
    if skip:
        skip = False
        continue
    if i == 0:
        continue
    if p in ('-o', '-c'):
        skip = (p == '-o')
        continue
    if p.endswith('.cpp') or p.endswith('.cpp.o'):
        continue
    if p.startswith('-M'):
        continue
    flags.append(p)

sc = root / 'scripts' / 'syntax-check.sh'
s = sc.read_text()
head, _, rest = s.partition('FLAGS=(\n')
_, _, tail = rest.partition(')\n')
sc.write_text(head + 'FLAGS=(\n' + ''.join('  ' + shlex.quote(f) + '\n' for f in flags) + ')\n' + tail)
print(f'refreshed {len(flags)} flags')
