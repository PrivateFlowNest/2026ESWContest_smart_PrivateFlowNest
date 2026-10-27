#!/usr/bin/env python3
"""Compatibility entry point. Contracts moved to <repo>/contracts (pfn.v1/) on 2026-10-21."""

from __future__ import annotations

import runpy
import sys
from pathlib import Path

VALIDATOR = Path(__file__).resolve().parents[3] / "contracts" / "validate-contracts.py"

if __name__ == "__main__":
    sys.argv[0] = str(VALIDATOR)
    runpy.run_path(str(VALIDATOR), run_name="__main__")
