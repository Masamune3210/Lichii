# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest

import cache_records as cr
import recover_cb1_candidates as cb
from test_cache_records import environment, log, record, cbuf_key


class RecoverCb1Tests(unittest.TestCase):
    def test_report_inventory(self):
        # Actual report reading is covered by the uploaded corpus audit.
        import json
        p = {'CfgStatus': 'blocked-missing-runtime-cb1', 'CfgBlockerDetail': 'BRX=0x30, CB1 byte offset=0x40, table entries=3',
             'Program': 'test', 'TitleIds': ['040074a01bf12000'], 'Stage': 'fragment',
             'NativeSha256': 'a'*64, 'HeaderSha256': 'b'*64, 'NativeBytes': 128}
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'report.json'
            path.write_text(json.dumps({'Programs': [p]}))
            req, = cb.read_requests(path)
            self.assertEqual((req['cb1_offset'], req['table_entries']), (64, 3))

    def test_only_exact_link_and_complete_words(self):
        title = '040074a01bf12000'
        shader = 0x12345678
        native = bytes(range(128))
        lowered = struct.pack('<2Q', 0x123456789ABCDEF0, 0xFEDCBA9876543210)
        program = cb.PROGRAM_HEADER.pack(1, 0, 0, 0, 4, 128, 0, 2, len(native)//16, 0, 2,
                                         0, 1, 128+len(native), shader, bytes(80)) + native + lowered
        def pipeline(values, shader_hash=shader):
            return struct.pack('<I', 1) + environment(4, values) + struct.pack('<6Q', 0, 0, 0, 0, 0, shader_hash)
        words = [(cbuf_key(1, 64), 16), (cbuf_key(1, 68), 32), (cbuf_key(1, 72), 48)]
        other = words[:-1] + [(cbuf_key(1, 72), 64)]
        data = log(record(1, 1, program), record(2, 2, pipeline(words)), record(2, 3, pipeline(words)),
                   record(2, 4, pipeline(words[:-1])), record(2, 5, pipeline(words, shader+1)),
                   record(2, 6, pipeline(other)), record(2, 7, pipeline(words[:-1] + [(cbuf_key(1, 72), 0)])))
        req = {'title_id': title, 'stage': 'fragment', 'native_sha256': hashlib.sha256(native).hexdigest(),
               'native_bytes': len(native), 'cb1_offset': 64, 'table_entries': 3, 'candidates': []}
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'cache.bin'
            path.write_bytes(data)
            cb.audit_cache(path, title, [req])
        self.assertEqual(len(req['candidates']), 3)
        self.assertEqual(req['candidates'][0]['words'], [16, 32, 48])
        self.assertEqual(req['candidates'][0]['pipeline_count'], 2)
        self.assertEqual(req['candidates'][1]['words'], [16, 32, 64])
        self.assertTrue(req['candidates'][2]['zero_or_invalid_target'])


if __name__ == '__main__':
    unittest.main()
