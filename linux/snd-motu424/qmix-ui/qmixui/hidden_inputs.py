from pathlib import Path
from typing import Any

from .flag_file import FlagFile, FlagFileError


INPUT_COUNT = 96


class HiddenInputStoreError(FlagFileError):
    pass


class HiddenInputStore:
    """Which input strips the Mix page leaves out, shared by all browsers."""

    def __init__(self, path: Path) -> None:
        self._flags = FlagFile(
            path, "hidden", INPUT_COUNT, "hidden-input", HiddenInputStoreError
        )

    def read(self) -> list[bool]:
        return self._flags.read()

    def set_inputs(self, inputs: Any, hidden: Any) -> list[bool]:
        if (
            not isinstance(inputs, list)
            or not inputs
            or any(
                isinstance(index, bool)
                or not isinstance(index, int)
                or not 0 <= index < INPUT_COUNT
                for index in inputs
            )
        ):
            raise ValueError("inputs must be a list of input indices from 0 to 95")
        if not isinstance(hidden, bool):
            raise ValueError("hidden must be a boolean")
        return self._flags.update(inputs, hidden)
