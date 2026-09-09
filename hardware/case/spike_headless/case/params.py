"""Load the real repo's PARAMS['trim'] dict unmodified.

This is the spike's only tie back to the actual generator: it imports
hardware/case/params_trim.py (which itself does `from params_current
import PARAMS`) exactly the way firefly_case.py/offline_stl_check.py do,
so every number below (outer_radius, window_dia, lip_r, screw positions,
...) is byte-for-byte the same value the real Fusion-driven generator
uses -- nothing here is retyped or approximated.
"""
import copy
import os
import sys

_CASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
if _CASE_DIR not in sys.path:
    sys.path.insert(0, _CASE_DIR)

from params_trim import PARAMS as _TRIM_PARAMS  # noqa: E402

PARAMS = copy.deepcopy(_TRIM_PARAMS)
