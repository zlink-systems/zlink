#!/usr/bin/env python3
from pathlib import Path
import sys
import unittest
shared=Path(__file__).resolve().parents[5]/"framework/perf-contract"
sys.path.insert(0,str(shared))
from test_contract import ContractTests
from test_runner import RunnerTests
if __name__=="__main__":unittest.main()
