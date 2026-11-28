import py_compile
from pathlib import Path

import pytest


PERF_DIR = Path(__file__).resolve().parents[1] / "perf"
PERF_SOURCES = sorted(PERF_DIR.rglob("*.py"))


@pytest.mark.parametrize(
    "source", PERF_SOURCES, ids=[source.relative_to(PERF_DIR).as_posix() for source in PERF_SOURCES]
)
def test_perf_source_compiles(source, tmp_path):
    py_compile.compile(str(source), cfile=str(tmp_path / "perf.pyc"), doraise=True)
