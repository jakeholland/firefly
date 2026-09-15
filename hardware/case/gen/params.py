"""Load the real repo's PARAMS dicts unmodified.

This is the port's only tie back to the actual Fusion generator: it
imports hardware/case/params_trim.py / params_current.py exactly the way
firefly_case.py/offline_stl_check.py do (the same sys.path trick), so
every number the headless build uses (outer_radius, window_dia, lip_r,
screw positions, ...) is byte-for-byte the same value the Fusion-driven
generator uses -- nothing here is retyped or approximated. See
docs/hardware/headless-port-plan.md for the per-function port status of
firefly_case.py itself.
"""
import copy
import os
import sys

_CASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
if _CASE_DIR not in sys.path:
    sys.path.insert(0, _CASE_DIR)

# Fusion's own firefly_case.py forces a fresh import every run (its own
# worktree-shadowing comment) -- mirror that here so a repeated build()
# in one process never serves a stale module from a different variant.
for _mod in ('params_current', 'params_trim'):
    sys.modules.pop(_mod, None)

from params_current import PARAMS as PARAMS_CURRENT  # noqa: E402
from params_trim import PARAMS as PARAMS_TRIM  # noqa: E402

VARIANTS = {'current': PARAMS_CURRENT, 'trim': PARAMS_TRIM}


def get_params(variant='trim'):
    """A fresh deep copy of the named variant's PARAMS dict -- callers are
    free to mutate their own copy without affecting later builds."""
    return copy.deepcopy(VARIANTS[variant])
