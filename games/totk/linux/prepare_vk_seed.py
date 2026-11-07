#!/usr/bin/env python3
"""The Linux runtime's prepared shader seed: the shaders of the verified local RomFS translated to SPIR-V through
totk_xlate --service --target vulkan, for the default bind state and every state the game's shared shader list
(games/botw/assets/shader_list.tsv, docs/shader-list.md) names, as scripts/prepare_shader_seed.py prepares the Metal
seed (its inventory, locators, shared-translation and job plan are used as they are).

A variant is <variant>.spv and <variant>.vk.txt; the "dep" lines of vk.txt are the answers its translation read
(tools/totk_xlate/Program.cs). <shader>.variants names a shader's variants, one a line. The runtime uses the first
variant whose deps the bound state answers the same way and translates only a state none serves
(runtime/linux/vk/vk_shader.c). A listed state an earlier variant of the shader already serves is not translated, and a
translation with the deps of an earlier variant is dropped. Vulkan samples a depth texture as any other, so the Metal
seed's depth-bound compare variants are not made. The output is game-derived data: it never goes into the repo.

usage: prepare_vk_seed.py --romfs <RomFS folder> --modules rewritten/<version> --translator <totk_xlate>
       --output <seed folder> [--game botw] [--shader-list <list>]
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
import prepare_shader_seed as seed   # noqa: E402
from shader_list import ShaderList   # noqa: E402

CHAIN = 'botw-vk-v2'   # vk_shader.c CHAIN
VK_TEX = ('1d', '1darray', 'buffer', '2darray', '2dms', '2dmsarray', '3d', 'cube', 'cubearray')


def vk_deps(path):
    """A variant's deps from its vk.txt: {tex, img, attr, fout: {slot: answer}, dmw: 0 / 1 (vertex)}."""
    deps = dict(tex={}, img={}, attr={}, fout={})
    for line in Path(path).read_text().splitlines():
        f = line.split()
        if f[:2] == ['dep', 'dmw']:
            deps['dmw'] = int(f[2])
        elif f[:1] == ['dep']:
            deps[f[1]][f[2]] = f[3]
    return deps


def vk_serves(deps, answers):
    """Whether a variant serves a bind state: the state answers every dep as the translator reads an answer (Program.cs
    Accessor: an unanswered texture slot is 2D or the image slot's type, an unanswered image format R8G8B8A8Unorm, an
    unanswered input or output float; vk_shader.c serves). translation_request asks with depth_minus_w."""
    tex, img = answers.get('tex', {}), answers.get('img', {})

    def tex_type(slot):
        t = tex.get(slot) or (img.get(slot) or {}).get('type') or '2d'
        t = t.removesuffix('_depth')
        return t if t in VK_TEX else '2d'

    def cls(kind, slot):
        v = answers.get(kind, {}).get(slot)
        return v if v in ('sint', 'uint') else 'float'

    got = dict(tex=tex_type, img=lambda slot: (img.get(slot) or {}).get('fmt') or 'R8G8B8A8Unorm',
               attr=lambda slot: cls('attr', slot), fout=lambda slot: cls('fout', slot))
    return deps.get('dmw', 1) == 1 and all(got[kind](slot) == want for kind in ('tex', 'img', 'attr', 'fout')
                                           for slot, want in deps[kind].items())


def prepare(romfs, modules, translator, output, game, shader_list):
    shared = ShaderList.load(shader_list)
    _, shaders, startup, archives = seed.inventory(romfs, modules, game)
    extra = seed.located({where for where in shared.locators if where[0] not in shaders},
                         seed.source_reader(romfs, modules, game), game)
    shaders.update(extra)
    print(f'{len(extra)} more shaders read where the shared list locates them', flush=True)
    same = {}
    for key in sorted(shaders):
        stage, control, code, _ = shaders[key]
        same.setdefault(seed.translation_input(stage, control, code), []).append(key)
    served = {key: keys[0] for keys in same.values() for key in keys[1:]}   # shader -> the shader translated for it
    jobs = seed.plan({key: shader for key, shader in shaders.items() if key not in served},
                     {served.get(key, key) for key in startup},
                     {(served.get(key, key), deps) for key, deps in shared.variants},
                     seed.GAMES[game]['all_default'])
    output.mkdir(parents=True)
    by_key = {}
    for key, answers, variant, why in jobs:
        by_key.setdefault(key, []).append((answers, variant, why))
    pending = iter(sorted(by_key.items()))
    lock = threading.Lock()
    produced, failed = {}, []
    done = [0, 0]   # variants kept, jobs a variant already served
    started = time.monotonic()

    def worker():
        process = subprocess.Popen([str(translator), '--service', '--target', 'vulkan', '--out', str(output),
                                    '--chain', CHAIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            while True:
                with lock:
                    item = next(pending, None)
                if item is None:
                    return
                key, todo = item
                stage, control, code, source = shaders[key]
                mine = {}   # canonical deps -> variant
                for answers, variant, why in todo:
                    if why != 'default' and any(vk_serves(json.loads(d), answers) for d in mine):
                        with lock:
                            done[1] += 1
                        continue
                    code_path = output/(variant+'.code')
                    request = seed.translation_request(variant, stage, control, code, answers, code_path)
                    process.stdin.write(json.dumps(request)+'\n'); process.stdin.flush()
                    reply = json.loads(process.stdout.readline())
                    code_path.unlink()
                    (output/(variant+'.res.json')).unlink(missing_ok=True)   # the runtime reads vk.txt
                    if not reply.get('ok'):
                        with lock:
                            failed.append(dict(key=key, variant=variant, why=why, stage=stage, source=source,
                                               error=reply.get('error')))
                        continue
                    deps = json.dumps(vk_deps(output/(variant+'.vk.txt')), sort_keys=True, separators=(',', ':'))
                    if deps in mine:
                        for end in ('.spv', '.vk.txt'):
                            (output/(variant+end)).unlink()
                        continue
                    mine[deps] = variant
                    with lock:
                        done[0] += 1
                        if done[0] % 500 == 0:
                            print(f'prepared {done[0]} variants of {len(jobs)} jobs; {time.monotonic()-started:.0f}s',
                                  flush=True)
                if mine:
                    (output/(key+'.variants')).write_text(''.join(v+'\n' for v in mine.values()))
                    with lock:
                        produced[key] = list(mine.values())
        finally:
            process.stdin.close(); process.wait()

    workers = min(8, os.cpu_count() or 4)
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        for future in [pool.submit(worker) for _ in range(workers)]:
            future.result()
    for key, first in sorted(served.items()):
        if first in produced:
            (output/(key+'.variants')).write_text((output/(first+'.variants')).read_text())
    report = dict(chain=CHAIN, game=game, discovered=len(shaders), located=len(extra), startup=len(startup),
                  served_by_another=sum(1 for first in served.values() if first in produced),
                  shaders=len(produced), variants=done[0], served_by_earlier_variant=done[1],
                  listed_jobs=sum(1 for *_, why in jobs if why == 'listed'), failed=failed, archives=archives,
                  seconds=round(time.monotonic()-started, 1))
    (output/'seed-manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f'Vulkan seed ready: {done[0]} variants of {len(produced)} shaders ({report["listed_jobs"]} listed states, '
          f'{done[1]} served by an earlier variant), {len(failed)} failed, {report["seconds"]}s', flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ['romfs', 'modules', 'translator', 'output']:
        p.add_argument('--'+name, type=Path, required=True)
    p.add_argument('--game', choices=sorted(seed.GAMES), default='botw')
    p.add_argument('--shader-list', type=Path, help="the shared shader list (default: the game's)")
    a = p.parse_args()
    prepare(a.romfs.resolve(), a.modules.resolve(), a.translator.resolve(), a.output.resolve(), a.game,
            (a.shader_list or seed.GAMES[a.game]['list']).resolve())


if __name__ == '__main__':
    main()
