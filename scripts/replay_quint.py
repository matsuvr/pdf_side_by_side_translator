"""Replay Quint ITF on the same snapshot ABI used by the two native viewers."""
import ctypes as c
import json
import os
import shutil
import time

from common import ROOT, sha256

FILES = ["P01-simple.pdf", "P04-crop.pdf", "P11-password.pdf", "P11-password.pdf", "missing.pdf"]
QUERIES = ["  ", "SENSORS", "samples", "no-such-phrase"]
IDLE, LOADING, READING_IMAGE, TRANSLATING, READY, EMPTY, ERROR, PASSWORD = range(8)


class Bytes(c.Structure):
    _fields_ = [("data", c.c_void_p), ("length", c.c_size_t)]

    def value(self):
        return c.string_at(self.data, self.length)


class View(c.Structure):
    _fields_ = [("revision", c.c_uint64), ("image_id", c.c_uint64),
                ("page", c.c_uint32), ("count", c.c_uint32), ("phase", c.c_uint32),
                ("source", Bytes), ("translation", Bytes), ("message", Bytes), ("png", Bytes),
                ("page_width", c.c_double), ("page_height", c.c_double)]


def bind(filename):
    lib = c.CDLL(filename)
    ptr, string = c.c_void_p, c.c_char_p
    for name, result, args in [
        ("reader_new", ptr, []), ("reader_free", None, [ptr]),
        ("reader_open", None, [ptr, string, string]), ("reader_find", None, [ptr, string]),
        ("reader_page", None, [ptr, c.c_uint32]), ("reader_snapshot_free", None, [ptr]),
        ("reader_poll", ptr, [ptr, c.c_uint64, c.c_uint64, c.POINTER(View)]),
    ]:
        fn = getattr(lib, name)
        fn.restype, fn.argtypes = result, args
    return lib


def check_view(view, expected):
    page, count = (int(expected[k]["#bigint"]) for k in ("page", "count"))
    assert (view.page, view.count) == (page, count), (view.page, view.count, expected)
    source = view.source.value().decode("utf-8")
    translation = view.translation.value().decode("utf-8")
    if page == 0:
        assert view.phase == {"idle": IDLE, "locked": PASSWORD, "missing": ERROR}[expected["pdf"]]
        assert not source and not translation and view.image_id == 0 and not view.png.length
    else:
        assert view.image_id and view.png.value().startswith(b"\x89PNG\r\n\x1a\n")
        assert view.page_width > 0 and view.page_height > 0
        marker = expected["marker"]
        if marker:
            assert marker in source, (marker, source)
            assert view.phase in (READY, ERROR)
            if view.phase == ERROR:
                # reader_new resolves the bundled model beside the test host.
                assert "アプリと同じフォルダーに" in view.message.value().decode("utf-8")
        else:  # Blank fixture page. System OCR can be unavailable.
            assert not source and view.phase in (EMPTY, ERROR)
        assert bool(translation.strip()) == (view.phase == READY)


def replay_trace(lib, trace, inputs):
    reader = lib.reader_new()
    if not reader:
        raise RuntimeError("reader_new failed")
    snapshot = None
    view = View()
    try:
        for index, state in enumerate(json.loads(trace.read_text(encoding="utf-8"))["states"]):
            op, arg = state["op"], int(state["argument"]["#bigint"])
            revision = view.revision
            old_source = view.source.value()
            if op == "open":
                lib.reader_open(reader, str(inputs / FILES[arg]).encode(),
                                b"reader-fixture" if arg == 3 else b"")
            elif op == "page":
                lib.reader_page(reader, arg)
            elif op == "find":
                lib.reader_find(reader, QUERIES[arg].encode())
            elif op == "replace":
                lib.reader_page(reader, 2)
                lib.reader_find(reader, b"samples")
                lib.reader_open(reader, str(inputs / FILES[1]).encode(), b"")
            elif op != "init":
                raise ValueError(f"Unknown trace operation {op}")

            if op == "init" or state["changed"]:
                after = 2**64 - 1 if op == "init" else revision
                deadline = time.monotonic() + 120
                while time.monotonic() < deadline:
                    candidate = View()
                    new = lib.reader_poll(reader, after, 0, c.byref(candidate))
                    if new:
                        try:
                            if state["searchMiss"]:
                                assert candidate.image_id == view.image_id, (
                                    f"search miss changed image ID {view.image_id} -> {candidate.image_id}; "
                                    "the viewer resets the reading position")
                                assert candidate.source.value() == old_source, "search miss replaced the source"
                                assert candidate.translation.value() == view.translation.value(), (
                                    "search miss replaced the translation")
                            after = candidate.revision
                            if candidate.phase not in (LOADING, READING_IMAGE, TRANSLATING):
                                # Previous snapshot remains readable across publication.
                                assert view.source.value() == old_source
                                lib.reader_snapshot_free(snapshot)
                                snapshot, view = new, candidate
                                new = None
                                break
                        finally:
                            lib.reader_snapshot_free(new)
                    time.sleep(0.01)
                else:
                    raise RuntimeError(f"Timed out at {trace.name} step {index} ({op} {arg})")
                assert op == "init" or view.revision > revision

            check_view(view, state["v"])
            probe = View()
            assert not lib.reader_poll(reader, view.revision, view.image_id, c.byref(probe))
            same_image = lib.reader_poll(reader, 2**64 - 1, view.image_id, c.byref(probe))
            try:
                assert same_image and probe.revision == view.revision and probe.png.length == 0
            finally:
                lib.reader_snapshot_free(same_image)
    except AssertionError as error:
        raise AssertionError(f"{trace.name} step {index} ({op} {arg}): {error}") from error
    finally:
        lib.reader_free(reader)
        lib.reader_snapshot_free(snapshot)


def fingerprint(directory):
    entries = {}
    for path in directory.rglob("*"):
        relative = path.relative_to(directory)
        entries[relative] = (path.stat().st_mtime_ns, sha256(path)) if path.is_file() else "directory"
    return entries


def replay(library, traces, directory):
    inputs = directory / "inputs"
    inputs.mkdir()
    for filename in set(FILES[:-1]):
        shutil.copyfile(ROOT / "tests/fixtures" / filename, inputs / filename)
    before = fingerprint(inputs)
    lib = bind(library)
    previous_cwd = os.getcwd()
    try:
        os.chdir(inputs)
        for trace in traces:
            replay_trace(lib, trace, inputs)
    finally:
        os.chdir(previous_cwd)
        assert fingerprint(inputs) == before, "Reader changed the input directory"
    print(f"Replayed {len(traces)} navigation traces through pdf_reader.")
