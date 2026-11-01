"""
Paths shown to the user are relative to the project root (e.g. Src_RL_Trading/RL_Agents/artifacts/rl), never absolute
The root is the closest parent folder holding both Src_RL_Trading/ and Data/
Used standalone (outside that project), paths are shown relative to the current folder when possible
"""
from __future__ import annotations

from pathlib import Path


def display_path(path) -> str:
    resolved = Path(path).resolve()
    for parent in [resolved, *resolved.parents]:
        if (parent / "Src_RL_Trading").is_dir() and (parent / "Data").is_dir():
            return str(resolved.relative_to(parent))
    try:
        return str(resolved.relative_to(Path.cwd()))
    except ValueError:
        return str(path)
