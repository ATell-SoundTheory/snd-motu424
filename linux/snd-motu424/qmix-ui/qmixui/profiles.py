import json
import os
import re
import tempfile
import time
import uuid
from pathlib import Path
from typing import Any


class ProfileStoreError(RuntimeError):
    pass


class ProfileStore:
    def __init__(self, directory: Path) -> None:
        self.directory = directory.expanduser()
        self.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chmod(self.directory, 0o700)

    def _path(self, profile_id: str) -> Path:
        try:
            normalized = str(uuid.UUID(profile_id))
        except ValueError as error:
            raise ProfileStoreError("invalid profile id") from error
        return self.directory / f"{normalized}.json"

    def list(self) -> list[dict[str, Any]]:
        profiles = []
        for path in self.directory.glob("*.json"):
            if not re.fullmatch(r"[0-9a-f-]{36}\.json", path.name):
                continue
            data = self._read(path)
            profiles.append(
                {
                    "id": path.stem,
                    "name": data["name"],
                    "updated": data["updated"],
                }
            )
        return sorted(profiles, key=lambda profile: profile["name"].casefold())

    def get(self, profile_id: str) -> dict[str, Any]:
        data = self._read(self._path(profile_id))
        data["id"] = profile_id
        return data

    def create(self, name: Any, snapshot: dict[str, Any]) -> dict[str, Any]:
        clean_name = self._validate_name(name)
        if len(self.list()) >= 100:
            raise ProfileStoreError("profile limit reached")
        profile_id = str(uuid.uuid4())
        data = {
            "id": profile_id,
            "name": clean_name,
            "updated": int(time.time()),
            "snapshot": snapshot,
        }
        self._write(self._path(profile_id), data)
        return {key: data[key] for key in ("id", "name", "updated")}

    def replace(self, profile_id: str, snapshot: dict[str, Any]) -> dict[str, Any]:
        path = self._path(profile_id)
        data = self._read(path)
        data["updated"] = int(time.time())
        data["snapshot"] = snapshot
        self._write(path, data)
        return {key: data[key] for key in ("id", "name", "updated")}

    def delete(self, profile_id: str) -> None:
        path = self._path(profile_id)
        try:
            path.unlink()
        except FileNotFoundError as error:
            raise ProfileStoreError("profile not found") from error

    def _read(self, path: Path) -> dict[str, Any]:
        try:
            if path.stat().st_size > 2_000_000:
                raise ProfileStoreError("profile file is too large")
            data = json.loads(path.read_text(encoding="utf-8"))
        except FileNotFoundError as error:
            raise ProfileStoreError("profile not found") from error
        except (json.JSONDecodeError, UnicodeDecodeError) as error:
            raise ProfileStoreError("profile file is invalid") from error
        if not isinstance(data, dict) or not isinstance(data.get("name"), str):
            raise ProfileStoreError("profile file is invalid")
        return data

    def _write(self, path: Path, data: dict[str, Any]) -> None:
        fd, temporary = tempfile.mkstemp(prefix=".profile-", dir=self.directory)
        try:
            os.fchmod(fd, 0o600)
            with os.fdopen(fd, "w", encoding="utf-8") as stream:
                json.dump(data, stream, separators=(",", ":"), ensure_ascii=True)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
        except OSError as error:
            try:
                os.unlink(temporary)
            except FileNotFoundError:
                pass
            raise ProfileStoreError(f"could not save profile: {error}") from error

    @staticmethod
    def _validate_name(name: Any) -> str:
        if not isinstance(name, str):
            raise ProfileStoreError("profile name must be text")
        clean = name.strip()
        if not clean or len(clean) > 48 or any(ord(char) < 32 for char in clean):
            raise ProfileStoreError("profile name must be 1–48 printable characters")
        return clean
