from pathlib import Path
from typing import Any

from .flag_file import FlagFile, FlagFileError


PAIR_COUNT = 48


class StereoPairStoreError(FlagFileError):
    pass


class StereoPairStore:
    def __init__(self, path: Path) -> None:
        self._flags = FlagFile(
            path, "pairs", PAIR_COUNT, "stereo-pair", StereoPairStoreError
        )

    def read(self) -> list[bool]:
        return self._flags.read()

    def linked_inputs(self) -> list[int]:
        """Return the left input index of every linked pair."""
        return [
            pair * 2 for pair, enabled in enumerate(self._flags.read()) if enabled
        ]

    def is_enabled(self, left_input: Any) -> bool:
        return self._flags.get(_pair_index(left_input))

    def set_pair(self, left_input: Any, enabled: Any) -> list[bool]:
        pair_index = _pair_index(left_input)
        if not isinstance(enabled, bool):
            raise ValueError("enabled must be a boolean")
        return self._flags.update([pair_index], enabled)


def _pair_index(left_input: Any) -> int:
    if (
        isinstance(left_input, bool)
        or not isinstance(left_input, int)
        or left_input < 0
        or left_input >= 96
        or left_input % 2
    ):
        raise ValueError("left_input must be an even input index from 0 to 94")
    return left_input // 2
