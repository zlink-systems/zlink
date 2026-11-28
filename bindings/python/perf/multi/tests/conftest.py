import sys
from pathlib import Path


MULTI_DIR = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(MULTI_DIR), str(MULTI_DIR.parent)]
