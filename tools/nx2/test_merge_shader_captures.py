import io
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

from merge_shader_captures import merge

STEM = "0123456789abcdef-gfx-1000"


def write_json(path, value):
    path.write_text(json.dumps(value), encoding="utf-8")


def session(root, name, label="Pokopia", cb1=b"\x10\0\0\0", header=b"\0"*128, mesh=0):
    run = root / name
    shaders = run / "shaders"
    shaders.mkdir(parents=True)
    write_json(run / "capture-manifest.json", {"schema": "lichii-native-shader-run-v1",
                "label": label, "finishedUtc": "2026-11-27T00:00:00Z"})
    (shaders / (STEM + ".bin")).write_bytes(bytes(16))
    (shaders / (STEM + ".header.bin")).write_bytes(header)
    write_json(shaders / (STEM + ".json"), {"schema": "lichii-native-ampere-v1",
               "title_id": "0400000000000000", "stage": "fs", "header_size": 128, "native_size": 16})
    cb1_name = STEM + "-original.cb1.bin" if cb1 else ""
    if cb1:
        (shaders / cb1_name).write_bytes(cb1)
    write_json(shaders / (STEM + "-original.context.json"), {"schema": "lichii-native-ampere-runtime-v1",
               "source_bin": STEM + ".bin", "title_id": "0400000000000000", "stage": "fs",
               "cb1_status": "captured" if cb1 else "unmapped", "cb1_file": cb1_name,
               "cb1_captured_bytes": len(cb1), "mesh_shader_a": mesh})
    return run


def archive(output, state):
    return zipfile.ZipFile(io.BytesIO(b"".join((output / p).read_bytes() for p in state["archives"])))


class CumulativeTests(unittest.TestCase):
    def test_duplicate_and_new_context(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            session(root, "one")
            out, state = merge(root, "Pokopia")
            self.assertEqual(len(state["program_ids"]), 1)
            session(root, "two")
            session(root, "three", cb1=b"\x20\0\0\0")
            out, state = merge(root, "Pokopia")
            self.assertEqual(len(state["program_ids"]), 1)
            self.assertEqual(len(state["context_ids"]), 2)
            with archive(out, state) as z:
                self.assertIsNone(z.testzip())
                index = json.loads(z.read("runtime-context-index.json"))
                self.assertEqual(sorted(len(c["origins"]) for c in index), [1, 2])
                for ctx in index:
                    meta = json.loads(z.read("shaders/" + ctx["contextFile"]))
                    self.assertIn("shaders/" + meta["source_bin"], z.namelist())
                    self.assertIn("shaders/" + meta["cb1_file"], z.namelist())

    def test_collision_missing_and_mesh_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            session(root, "one", cb1=b"")
            session(root, "two", cb1=b"", mesh=1)
            session(root, "three")
            session(root, "four", header=b"\x01"*128)
            out, state = merge(root, "Pokopia", part_limit=200)
            self.assertEqual(len(state["program_ids"]), 2)
            self.assertEqual(len(state["context_ids"]), 4)
            self.assertGreater(len(state["archives"]), 1)
            self.assertTrue(all((out / p).stat().st_size <= 200 for p in state["archives"]))
            with archive(out, state) as z:
                self.assertIsNone(z.testzip())

    def test_import_persists_and_failure_keeps_previous(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            run = session(root, "other", label="Lichii")
            out, state = merge(root, "Pokopia", [run])
            _, again = merge(root, "Pokopia")
            self.assertEqual(state["program_ids"], again["program_ids"])
            original = (out / again["archives"][0]).read_bytes()
            (run / "shaders" / (STEM + ".bin")).unlink()
            with self.assertRaises(ValueError):
                merge(root, "Pokopia")
            self.assertEqual((out / again["archives"][0]).read_bytes(), original)

    def test_malformed_snapshot_preserves_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            session(root, "one")
            out, state = merge(root, "Pokopia")
            before = (out / state["archives"][0]).read_bytes()
            run = session(root, "two")
            (run / "shaders" / (STEM + "-original.cb1.bin")).write_bytes(b"\0")
            with self.assertRaises(ValueError):
                merge(root, "Pokopia")
            self.assertEqual((out / state["archives"][0]).read_bytes(), before)

    def test_no_other_label_implicitly_imported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            session(root, "other", label="Lichii")
            with self.assertRaises(ValueError):
                merge(root, "Pokopia")
            self.assertFalse((root / "Pokopia-cumulative").exists())


if __name__ == "__main__":
    unittest.main()
