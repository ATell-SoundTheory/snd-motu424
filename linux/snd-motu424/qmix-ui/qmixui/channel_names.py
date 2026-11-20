import json
import os
import tempfile
import threading
from pathlib import Path
from typing import Any


INPUT_COUNT = 96
OUTPUT_PAIR_COUNT = 48
MAX_NAME_LENGTH = 48
MAX_FILE_SIZE = 128 * 1024


class ChannelNameValidationError(ValueError):
    pass


class ChannelNameStoreError(RuntimeError):
    pass


class ChannelNameStore:
    def __init__(self, path: Path) -> None:
        self.path = path.expanduser()
        try:
            self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            os.chmod(self.path.parent, 0o700)
        except OSError as error:
            raise ChannelNameStoreError(
                f"could not prepare channel-name storage: {error}"
            ) from error
        self._lock = threading.RLock()
        self._names = self._read_file()

    def read(self) -> dict[str, list[str]]:
        with self._lock:
            return {
                "inputs": self._names["inputs"].copy(),
                "outputs": self._names["outputs"].copy(),
            }

    def replace(self, inputs: Any, outputs: Any) -> dict[str, list[str]]:
        names = {
            "inputs": self._validate_names(inputs, INPUT_COUNT, "inputs"),
            "outputs": self._validate_names(outputs, OUTPUT_PAIR_COUNT, "outputs"),
        }
        with self._lock:
            self._write_file(names)
            self._names = names
            return self.read()

    def _read_file(self) -> dict[str, list[str]]:
        try:
            if self.path.stat().st_size > MAX_FILE_SIZE:
                raise ChannelNameStoreError("channel-name file is too large")
            data = json.loads(self.path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return {
                "inputs": [""] * INPUT_COUNT,
                "outputs": [""] * OUTPUT_PAIR_COUNT,
            }
        except (json.JSONDecodeError, UnicodeDecodeError) as error:
            raise ChannelNameStoreError("channel-name file is invalid") from error
        except OSError as error:
            raise ChannelNameStoreError(
                f"could not read channel-name file: {error}"
            ) from error
        if not isinstance(data, dict):
            raise ChannelNameStoreError("channel-name file is invalid")
        try:
            return {
                "inputs": self._validate_names(data.get("inputs"), INPUT_COUNT, "inputs"),
                "outputs": self._validate_names(
                    data.get("outputs"), OUTPUT_PAIR_COUNT, "outputs"
                ),
            }
        except ChannelNameValidationError as error:
            raise ChannelNameStoreError("channel-name file is invalid") from error

    def _write_file(self, names: dict[str, list[str]]) -> None:
        fd, temporary = tempfile.mkstemp(
            prefix=".channel-names-", dir=self.path.parent
        )
        try:
            os.fchmod(fd, 0o600)
            with os.fdopen(fd, "w", encoding="utf-8") as stream:
                json.dump(names, stream, separators=(",", ":"), ensure_ascii=True)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, self.path)
        except OSError as error:
            try:
                os.unlink(temporary)
            except FileNotFoundError:
                pass
            raise ChannelNameStoreError(
                f"could not save channel names: {error}"
            ) from error

    @staticmethod
    def _validate_names(value: Any, count: int, field: str) -> list[str]:
        if not isinstance(value, list) or len(value) != count:
            raise ChannelNameValidationError(
                f"{field} must contain exactly {count} names"
            )
        names = []
        for index, name in enumerate(value):
            if not isinstance(name, str):
                raise ChannelNameValidationError(
                    f"{field}[{index}] must be text"
                )
            if len(name) > MAX_NAME_LENGTH or any(
                ord(character) < 32 or ord(character) == 127
                for character in name
            ):
                raise ChannelNameValidationError(
                    f"{field}[{index}] must be at most {MAX_NAME_LENGTH} printable characters"
                )
            names.append(name.strip())
        return names
