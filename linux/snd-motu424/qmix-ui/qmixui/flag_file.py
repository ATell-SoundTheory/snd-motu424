import json
import os
import tempfile
import threading
from collections.abc import Iterable
from pathlib import Path


MAX_FILE_SIZE = 4096


class FlagFileError(RuntimeError):
    pass


class FlagFile:
    """A fixed-length list of booleans kept in a small JSON file."""

    def __init__(
        self,
        path: Path,
        key: str,
        count: int,
        label: str,
        error: type[FlagFileError] = FlagFileError,
    ) -> None:
        self.path = path.expanduser()
        self._key = key
        self._count = count
        self._label = label
        self._error = error
        try:
            self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            os.chmod(self.path.parent, 0o700)
        except OSError as cause:
            raise error(f"could not prepare {label} storage: {cause}") from cause
        self._lock = threading.RLock()
        self._flags = self._read_file()

    def read(self) -> list[bool]:
        with self._lock:
            return self._flags.copy()

    def get(self, index: int) -> bool:
        with self._lock:
            return self._flags[index]

    def update(self, indices: Iterable[int], enabled: bool) -> list[bool]:
        with self._lock:
            flags = self._flags.copy()
            for index in indices:
                flags[index] = enabled
            if flags != self._flags:
                self._write_file(flags)
                self._flags = flags
            return self.read()

    def _read_file(self) -> list[bool]:
        invalid = f"{self._label} file is invalid"
        try:
            if self.path.stat().st_size > MAX_FILE_SIZE:
                raise self._error(f"{self._label} file is too large")
            data = json.loads(self.path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return [False] * self._count
        except (json.JSONDecodeError, UnicodeDecodeError) as cause:
            raise self._error(invalid) from cause
        except OSError as cause:
            raise self._error(
                f"could not read {self._label} file: {cause}"
            ) from cause
        flags = data.get(self._key) if isinstance(data, dict) else None
        if (
            not isinstance(flags, list)
            or len(flags) != self._count
            or any(not isinstance(value, bool) for value in flags)
        ):
            raise self._error(invalid)
        return flags

    def _write_file(self, flags: list[bool]) -> None:
        fd, temporary = tempfile.mkstemp(
            prefix=f".{self.path.stem}-", dir=self.path.parent
        )
        try:
            os.fchmod(fd, 0o600)
            with os.fdopen(fd, "w", encoding="utf-8") as stream:
                json.dump({self._key: flags}, stream, separators=(",", ":"))
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, self.path)
        except OSError as cause:
            try:
                os.unlink(temporary)
            except FileNotFoundError:
                pass
            raise self._error(f"could not save {self._label} file: {cause}") from cause
