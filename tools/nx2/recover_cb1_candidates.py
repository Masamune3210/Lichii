#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read existing Lichii v2 caches and find recorded CB1 jump-table candidates.

Candidate evidence only: no native capture or existing cache is modified. The
pipeline environment contains recorded words, not their original mapping proof;
the Program record contains the converted 80-byte SPH, not the raw 128-byte
header. The existing cache reader checks header CRCs, not XXH3 payload hashes.
Never promote this JSON to a verified runtime capture automatically.
"""

import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import zipfile
import zlib

import cache_records as cr

PROGRAM = 1
PROGRAM_HEADER = struct.Struct('<IBBH7I4Q80s')
BLOCKER = re.compile(r'^BRX=0x([0-9a-f]+), CB1 byte offset=0x([0-9a-f]+), table entries=(\d+)$', re.I)
TITLE = re.compile(r'^[0-9a-f]{16}$', re.I)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_requests(path):
    if path.is_dir():
        reports = [json.loads(p.read_bytes()) for p in sorted(path.glob('AmpereNativeDecoderPreflight-*.json'))]
    elif zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            reports = [json.loads(archive.read(name)) for name in archive.namelist()
                       if Path(name).name.startswith('AmpereNativeDecoderPreflight-') and name.endswith('.json')]
    else:
        reports = [json.loads(path.read_bytes())]
    if not reports:
        raise ValueError('no corpus reports found')
    groups = {}
    for report in reports:
        for p in report['Programs']:
            if p.get('CfgStatus') != 'blocked-missing-runtime-cb1':
                continue
            match = BLOCKER.fullmatch(p['CfgBlockerDetail'])
            if not match:
                raise ValueError(f"unrecognized CB1 blocker: {p['Program']}")
            brx, offset = (int(match[i], 16) for i in (1, 2))
            count = int(match[3])
            if not 0 < count <= 16384 or offset & 3 or offset + count * 4 > 65536:
                raise ValueError(f"invalid CB1 range: {p['Program']}")
            for title in p['TitleIds']:
                key = (title.lower(), p['Stage'], p['NativeSha256'], p['HeaderSha256'], brx, offset, count)
                group = groups.setdefault(key, {'title_id': key[0], 'stage': key[1],
                    'native_sha256': key[2], 'header_sha256_unverified': key[3],
                    'native_bytes': p['NativeBytes'], 'brx_pc': brx, 'cb1_offset': offset,
                    'table_entries': count, 'programs': [], 'candidates': []})
                group['programs'].append(p['Program'])
    return list(groups.values())


def read_program(payload):
    (version, source, geometry, reserved, stage, header_size, _tess, _texture, native_count,
     trap_count, code_count, _address, _stamp, source_size, unique_hash, _sph) = PROGRAM_HEADER.unpack_from(payload)
    native_bytes = source_size - header_size
    start = PROGRAM_HEADER.size
    end = start + native_count * 16
    code_start = end + trap_count * 4
    if (version != 1 or reserved or source not in (0, 1) or stage not in cr.STAGES or
            header_size != (0 if source == 1 else 128) or
            (stage == cr.STAGE_COMPUTE) != (source == 1) or
            not 0 < native_bytes <= native_count * 16 or native_bytes % 16 or
            not code_count or code_start + code_count * 8 != len(payload)):
        raise cr.Damaged('invalid native Program record')
    return stage, digest(payload[start:start + native_bytes]), unique_hash, digest(payload[code_start:])


def normalize_stage(stage):
    return {'vertex_a': 'vertex', 'vertex_b': 'vertex', 'tess_control': 'tessellation-control',
            'tess_eval': 'tessellation-evaluation'}.get(cr.STAGES[stage], cr.STAGES[stage])


def audit_cache(path, title, requests):
    data = path.read_bytes()
    if (len(data) < cr.FILE_HEADER_SIZE or data[:8] != cr.FILE_MAGIC or
            struct.unpack_from('<II', data, 8) != (cr.FORMAT_VERSION, cr.FILE_HEADER_SIZE) or
            struct.unpack_from('<I', data, 24)[0] != zlib.crc32(data[:24])):
        raise cr.Damaged('invalid v2 file header')
    cache_sha = digest(data)
    wanted = collections.defaultdict(list)
    for request in requests:
        if request['title_id'] == title:
            wanted[(request['stage'], request['native_sha256'])].append(request)
    live = cr.parse_log(data, kinds=(PROGRAM, cr.KIND_GRAPHICS, cr.KIND_COMPUTE))
    programs = collections.defaultdict(list)
    bad = 0
    for kind, key, payload in live:
        if kind != PROGRAM:
            continue
        try:
            stage, native_sha, unique_hash, lowered_sha = read_program(payload)
            for request in wanted.get((normalize_stage(stage), native_sha), ()):
                programs[(stage, unique_hash, lowered_sha)].append((key, request))
        except (cr.Damaged, struct.error):
            bad += 1
    for kind, key, payload in live:
        if kind == PROGRAM:
            continue
        try:
            record = cr.read_record(kind, key, payload, include_code_hash=True)
        except (cr.Damaged, struct.error):
            bad += 1
            continue
        for env, shader in zip(record['environments'], record['shaders']):
            for program_key, request in programs.get((env['stage'], int(shader, 16), env['code_sha256']), ()):
                offset = request['cb1_offset']
                keys = [(1 << 32) | (offset + 4*i) for i in range(request['table_entries'])]
                values = env['cbuf_values']
                if not all(k in values for k in keys):
                    continue
                words = [values[k] for k in keys]
                candidate = {'cache_sha256': cache_sha, 'program_record_key': f'{program_key:016x}',
                    'lowered_sha256': env['code_sha256'], 'shader_hash': shader,
                    'words': words, 'zero_or_invalid_target': any(w == 0 or w % 16 or w >= request['native_bytes'] for w in words),
                    'pipeline_records': [f'{key:016x}'], 'pipeline_count': 1}
                # Deduplicate equal tables within one exact cache/program variant;
                # retain alternate table values and a bounded sample of provenance.
                existing = next((c for c in request['candidates'] if all(c[k] == candidate[k] for k in
                    ('cache_sha256', 'program_record_key', 'lowered_sha256', 'words'))), None)
                if existing is None:
                    request['candidates'].append(candidate)
                else:
                    existing['pipeline_count'] += 1
                    if len(existing['pipeline_records']) < 8:
                        existing['pipeline_records'].append(f'{key:016x}')
    return {'title_id': title, 'cache_sha256': cache_sha, 'live_records': len(live), 'unparsed_records': bad,
            'matched_program_record_links': sum(map(len, programs.values()))}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reports', type=Path, help='corpus report ZIP, JSON, or report directory')
    parser.add_argument('cache', type=Path, help='shader-cache root or individual lichii-cache.v2.bin')
    parser.add_argument('--title', type=str.lower, help='explicit title ID for an individual cache file')
    parser.add_argument('--output', type=Path, default=Path('cb1-cache-candidates.json'))
    args = parser.parse_args(argv)
    requests = read_requests(args.reports)
    if args.title and (not TITLE.fullmatch(args.title) or not args.cache.is_file()):
        parser.error('--title requires a 16-digit hex ID and an individual cache file')
    files = [args.cache] if args.cache.is_file() else sorted(args.cache.rglob('lichii-cache.v2.bin'))
    summaries, errors = [], []
    for path in files:
        title = args.title or next((p.name.lower() for p in path.parents if TITLE.fullmatch(p.name)), None)
        if not title:
            errors.append({'cache': str(path), 'error': 'no title ID directory; use --title for one file'})
            continue
        if not any(r['title_id'] == title for r in requests):
            continue
        try:
            summaries.append(audit_cache(path, title, requests))
        except (OSError, cr.Damaged, struct.error) as error:
            errors.append({'title_id': title, 'error': str(error)})
    result = {'schema': 'lichii-cb1-cache-candidates-v1', 'verified_runtime_capture': False,
              'payload_xxh3_verified': False, 'raw_header_verified': False,
              'caches': summaries, 'errors': errors, 'requests': requests}
    with args.output.open('x', encoding='utf-8') as out:
        json.dump(result, out, indent=2)
        out.write('\n')
    matched = [r for r in requests if r['candidates']]
    print(f'{len(summaries)} caches, {len(matched)}/{len(requests)} request groups with candidate tables; '
          f'{sum(len(r["programs"]) for r in matched)} blocked entries have candidates. {len(errors)} cache errors.')
    print(f'Candidate-only output: {args.output}')
    return int(not summaries or bool(errors))


if __name__ == '__main__':
    sys.exit(main())
