#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a cumulative, collision-safe native shader archive from completed raw runs.

Raw sessions are immutable inputs. A failed merge cannot overwrite an existing
archive. The label identifies a collection, not necessarily a single title.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import zipfile

PART_LIMIT = 45_000_000
LABEL = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}\Z")
STEM = re.compile(r"[0-9a-fA-F]{16}-(?:gfx|cs)-[0-9a-fA-F]+\Z")
RUN_SCHEMA = "lichii-native-shader-run-v1"
STATE_SCHEMA = "lichii-native-shader-cumulative-v1"
NATIVE_SCHEMA = "lichii-native-ampere-v1"
CONTEXT_SCHEMA = "lichii-native-ampere-runtime-v1"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def read_json(path):
    with path.open(encoding="utf-8-sig") as stream:
        return json.load(stream)


def companion(folder, name):
    if not isinstance(name, str) or not name or name in (".", "..") or any(c in name for c in "/\\:"):
        raise ValueError(f"invalid companion name {name!r} in {folder}")
    return (folder / name).read_bytes()


def add_json(archive, name, value):
    archive.writestr(name, encoded(value) + b"\n")


def add_origin(record, session, name):
    origin = {"session": session, "original_file": name}
    if origin not in record["origins"]:
        record["origins"].append(origin)


def collect_run(archive, run, session, programs, contexts, warnings):
    folder = run / "shaders"
    if not folder.is_dir():
        raise ValueError(f"missing shader directory: {folder}")
    sources = {}
    for path in sorted(folder.glob("*.json")):
        if path.name.endswith(".context.json"):
            continue
        stem = path.stem
        if not STEM.fullmatch(stem):
            continue
        meta = read_json(path)
        if meta.get("schema") != NATIVE_SCHEMA:
            raise ValueError(f"invalid native metadata: {path}")
        body_path = folder / (stem + ".bin")
        header_path = folder / (stem + ".header.bin")
        header_size = meta["header_size"]
        if header_size not in (0, 128) or (header_size == 0) != ("-cs-" in stem):
            raise ValueError(f"invalid header size: {path}")
        if not body_path.is_file() or (header_size and not header_path.is_file()):
            warnings.append({"session": session, "file": path.name, "reason": "missing native body/header"})
            continue
        body = body_path.read_bytes()
        header = header_path.read_bytes() if header_size else b""
        if not body or len(body) % 16 or len(body) != meta["native_size"] or len(header) != header_size:
            raise ValueError(f"invalid native body/header length: {path}")
        body_hash, header_hash = digest(body), digest(header)
        # Keep the original address, title and stage in the identity. Matching
        # bytes alone cannot prove that contexts from different bindings agree.
        identity = digest(encoded([stem.lower(), meta, body_hash, header_hash]))
        canonical = stem.lower() + "-" + identity
        sources[stem + ".bin"] = (identity, canonical)
        if identity not in programs:
            archive.writestr("shaders/" + canonical + ".bin", body)
            if header_size:
                archive.writestr("shaders/" + canonical + ".header.bin", header)
            add_json(archive, "shaders/" + canonical + ".json", meta)
            programs[identity] = {"source": canonical + ".bin", "body_sha256": body_hash,
                                  "header_sha256": header_hash, "title_id": meta["title_id"],
                                  "stage": meta["stage"], "origins": []}
        add_origin(programs[identity], session, path.name)

    for path in sorted(folder.glob("*.context.json")):
        meta = read_json(path)
        if meta.get("schema") != CONTEXT_SCHEMA:
            raise ValueError(f"invalid context metadata: {path}")
        source_name = meta["source_bin"]
        if source_name not in sources:
            warnings.append({"session": session, "file": path.name, "reason": "no complete native source"})
            continue
        source_id, stem = sources[source_name]
        if not path.name.startswith(source_name[:-4] + "-"):
            raise ValueError(f"context does not match source: {path}")
        status = meta["cb1_status"]
        cb1_name = meta.get("cb1_file", "")
        primitive_name = meta.get("primitive_header_file", "")
        cb1 = companion(folder, cb1_name) if cb1_name else b""
        primitive = companion(folder, primitive_name) if primitive_name else b""
        if status in ("captured", "captured-prefix"):
            if not cb1 or len(cb1) % 4 or len(cb1) > 65536 or len(cb1) != meta["cb1_captured_bytes"]:
                raise ValueError(f"invalid CB1 snapshot: {path}")
            if status == "captured-prefix" and len(cb1) >= min(meta["cb1_declared_bytes"], 65536):
                raise ValueError(f"invalid CB1 prefix: {path}")
        elif status not in ("unbound", "empty", "unmapped") or cb1 or meta["cb1_captured_bytes"] != 0:
            raise ValueError(f"invalid missing-CB1 context: {path}")
        if primitive_name and len(primitive) != 80:
            raise ValueError(f"invalid primitive header: {path}")
        # File names are references, not state. Deduplicate exact semantic
        # metadata and companion bytes, preserving all alternative contexts.
        normalized = dict(meta)
        for key in ("source_bin", "cb1_file", "primitive_header_file"):
            normalized.pop(key, None)
        cb1_hash = digest(cb1) if cb1 else None
        primitive_hash = digest(primitive) if primitive else None
        identity = digest(encoded([source_id, normalized, cb1_hash, primitive_hash]))
        name = stem + "-" + identity
        if identity not in contexts:
            meta["source_bin"] = stem + ".bin"
            meta["cb1_file"] = name + ".cb1.bin" if cb1 else ""
            meta["primitive_header_file"] = name + ".primitive-header.bin" if primitive else ""
            if cb1:
                archive.writestr("shaders/" + meta["cb1_file"], cb1)
            if primitive:
                archive.writestr("shaders/" + meta["primitive_header_file"], primitive)
            add_json(archive, "shaders/" + name + ".context.json", meta)
            contexts[identity] = {
                "titleId": meta["title_id"], "stage": meta["stage"],
                "sourceBin": meta["source_bin"], "sourceSha256": programs[source_id]["body_sha256"],
                "headerSha256": programs[source_id]["header_sha256"],
                "contextFile": name + ".context.json", "cb1Status": status,
                "cb1File": meta["cb1_file"] or None, "cb1Bytes": len(cb1),
                "cb1Sha256": cb1_hash, "primitiveSha256": primitive_hash, "origins": [],
            }
        add_origin(contexts[identity], session, path.name)


def split_archive(path, part_limit=PART_LIMIT):
    if path.stat().st_size <= part_limit:
        return [path.name]
    names = []
    expected = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            block = source.read(min(part_limit, 1024 * 1024))
            if not block:
                break
            if len(names) == 999:
                raise ValueError("more than 999 ZIP parts required")
            part = path.with_name(f"{path.name}.{len(names)+1:03d}")
            names.append(part.name)
            with part.open("xb") as out:
                written = 0
                while block:
                    out.write(block)
                    expected.update(block)
                    written += len(block)
                    if written == part_limit:
                        break
                    block = source.read(min(part_limit - written, 1024 * 1024))
    actual = hashlib.sha256()
    for name in names:
        with (path.parent / name).open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                actual.update(block)
    if actual.digest() != expected.digest():
        raise ValueError("split ZIP verification failed")
    path.unlink()
    return names


def completed_run(path):
    manifest = read_json(path / "capture-manifest.json")
    if manifest.get("schema") != RUN_SCHEMA or not manifest.get("finishedUtc"):
        raise ValueError(f"not a completed capture session: {path}")
    return manifest


def merge(root, label, imports=(), part_limit=PART_LIMIT):
    if not LABEL.fullmatch(label):
        raise ValueError("invalid label")
    if part_limit < 1:
        raise ValueError("invalid part limit")
    root = root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    output = root / (label + "-cumulative")
    backup = root / (label + "-cumulative.previous")
    lock = root / (label + "-cumulative.lock")
    # Exclusive writer, including separate launcher processes.
    with lock.open("x") as handle:
        handle.write(str(os.getpid()))
    staging = None
    try:
        if backup.exists():
            # A killed process may have stopped between directory renames.
            # Never guess which generation to delete.
            raise ValueError(f"previous generation exists: {backup}; inspect before retrying")
        old = read_json(output / "cumulative-state.json") if output.exists() else None
        if old is not None and (old.get("schema") != STATE_SCHEMA or old.get("label", "").casefold() != label.casefold()):
            raise ValueError(f"unrecognized existing cumulative archive: {output}")
        paths = {Path(p).resolve() for p in (old["runs"] if old else [])}
        paths.update(Path(p).resolve() for p in imports)
        for run in root.iterdir():
            if not run.is_dir() or not (run / "capture-manifest.json").is_file():
                continue
            manifest = read_json(run / "capture-manifest.json")
            if manifest.get("schema") == RUN_SCHEMA and str(manifest.get("label", "")).casefold() == label.casefold():
                paths.add(run.resolve())
        if not paths:
            raise ValueError(f"no completed sessions for label {label!r}; use --import-run for an existing run with another label")
        sessions = []
        for run in sorted(paths, key=lambda p: str(p).casefold()):
            manifest = completed_run(run)
            session = digest(encoded([str(run), manifest]))
            sessions.append((run, session, manifest))
        staging = Path(tempfile.mkdtemp(prefix=label + "-cumulative.tmp-", dir=root))
        programs, contexts, warnings = {}, {}, []
        archive_path = staging / (label + ".zip")
        with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9,
                             allowZip64=True) as archive:
            for run, session, _ in sessions:
                collect_run(archive, run, session, programs, contexts, warnings)
            if not programs:
                raise ValueError("no complete native shader captures found")
            if old and (not set(old["program_ids"]) <= programs.keys() or not set(old["context_ids"]) <= contexts.keys()):
                raise ValueError("prior capture records are missing or changed; refusing to lose existing corpus data")
            summary = {"schema": STATE_SCHEMA, "label": label,
                       "sessions": [{"id": session, "name": run.name, "revision": manifest.get("revision"),
                                     "label": manifest.get("label"), "finishedUtc": manifest["finishedUtc"]}
                                    for run, session, manifest in sessions],
                       "nativeShaderCaptures": len(programs), "runtimeContextCaptures": len(contexts),
                       "recordedCb1Contexts": sum(c["cb1File"] is not None for c in contexts.values()),
                       "titleIds": sorted({p["title_id"] for p in programs.values()} | {c["titleId"] for c in contexts.values()}),
                       "omitted_incomplete_records": warnings}
            add_json(archive, "capture-manifest.json", summary)
            add_json(archive, "runtime-context-index.json", list(contexts.values()))
            add_json(archive, "capture-provenance.json", {"programs": programs, "contexts": contexts})
        with zipfile.ZipFile(archive_path) as archive:
            bad = archive.testzip()
            if bad is not None:
                raise ValueError(f"ZIP CRC failed: {bad}")
        parts = split_archive(archive_path, part_limit)
        state = {"schema": STATE_SCHEMA, "label": label, "runs": [str(run) for run, _, _ in sessions],
                 "program_ids": sorted(programs), "context_ids": sorted(contexts), "archives": parts,
                 "summary": summary}
        (staging / "cumulative-state.json").write_bytes(encoded(state) + b"\n")
        if output.exists():
            output.rename(backup)
        try:
            staging.rename(output)
        except BaseException:
            if backup.exists() and not output.exists():
                backup.rename(output)
            raise
        if backup.exists():
            shutil.rmtree(backup)
        return output, state
    finally:
        if staging is not None and staging.exists():
            shutil.rmtree(staging)
        lock.unlink()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--label", required=True)
    parser.add_argument("--import-run", action="append", type=Path, default=[])
    args = parser.parse_args(argv)
    try:
        output, state = merge(args.root, args.label, args.import_run)
    except (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile) as error:
        print(f"Cumulative capture failed: {error}", file=sys.stderr)
        return 1
    summary = state["summary"]
    print(f'Cumulative capture: {len(state["runs"])} sessions, {summary["nativeShaderCaptures"]} native records, '
          f'{summary["runtimeContextCaptures"]} contexts, {summary["recordedCb1Contexts"]} recorded CB1 contexts')
    if summary["omitted_incomplete_records"]:
        print(f'Warning: {len(summary["omitted_incomplete_records"])} incomplete records omitted; original raw sessions retained.')
    for name in state["archives"]:
        print(f"  {output / name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
