"""Small bridge to tools/offline_stl_check.py's own overhang whitelist
constants, so the CLI/tests don't need to know that module's on-disk
path."""
from .gates import offline_stl_check


def top_whitelist():
    return list(getattr(offline_stl_check, 'TOP_WL', []))


def bottom_whitelist():
    return list(getattr(offline_stl_check, 'BOTTOM_WL', []))
