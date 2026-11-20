import argparse
import hashlib
import hmac
import http.cookies
import http.server
import ipaddress
import json
import mimetypes
import secrets
import ssl
import threading
import time
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, unquote, urlsplit

from .alsa import AlsaControl, AlsaError
from .channel_names import ChannelNameStore, ChannelNameStoreError
from .devices import group_devices, validate_setting
from .flag_file import FlagFileError
from .hidden_inputs import HiddenInputStore
from .mixer import (
    BUSES,
    PAIR_SHARED_FIELDS,
    ControlIO,
    MixerError,
    ProfileApplyError,
    StereoPairSyncError,
    apply_profile,
    capture_profile,
    label_inputs,
    profile_difference_count,
    read_bus_state,
    read_control,
    read_input_states,
    read_meters,
    read_topology,
    set_control,
    set_pair_control,
    sync_stereo_pair as sync_input_pair,
    validate_profile,
)
from .profiles import ProfileStore, ProfileStoreError
from .stereo_pairs import StereoPairStore


ROOT = Path(__file__).resolve().parents[1]
WEB_ROOT = ROOT / "web" / "dist"
SESSION_COOKIE = "motu424_session"
SESSION_LIFETIME = 12 * 60 * 60
MAX_BODY = 64 * 1024


class Device:
    def __init__(self, card: str, io: ControlIO | None = None) -> None:
        self.card = card
        self.io = io or AlsaControl(card)
        self.lock = threading.RLock()
        self.balance_hints: dict[tuple[int, int], float] = {}

    def close(self) -> None:
        self.io.close()

    def status(self, bus: int, linked_pairs: list[int] | None = None) -> dict[str, Any]:
        linked_pairs = linked_pairs or []
        with self.lock:
            topology = self._read_topology()
            if 0 <= bus < BUSES:
                for left_input in linked_pairs:
                    sync_input_pair(self.io, left_input, [bus])
            return read_bus_state(
                self.io, bus, topology, linked_pairs, self.balance_hints
            )

    def meters(self) -> dict[str, Any]:
        with self.lock:
            return read_meters(self.io)

    def topology(self) -> dict[str, Any]:
        with self.lock:
            return self._read_topology()

    def devices(self) -> dict[str, Any]:
        with self.lock:
            return group_devices(self.io.hardware_controls(), self._read_topology())

    def write_device_setting(self, name: Any, index: Any, values: Any) -> None:
        if not isinstance(name, str) or isinstance(index, bool) or not isinstance(index, int):
            raise MixerError("hardware setting requires a name and integer index")
        with self.lock:
            devices = group_devices(self.io.hardware_controls(), self._read_topology())
            for device in devices["devices"]:
                for control in device["controls"]:
                    if control["name"] == name and control["index"] == index:
                        checked = validate_setting(control, values)
                        self.io.write_hardware_control(name, index, checked, control["kind"])
                        return
        raise MixerError("hardware setting is unavailable")

    def _read_topology(self) -> dict[str, Any]:
        topology = read_topology(self.card)
        for port in topology["ports"]:
            if port["name"] != "2408" or port["configured"] is not True:
                continue
            port["analog_bank"] = self.io.read(
                "Interface 2408 Analog Bank", port["index"]
            )
            port["bank_formats"] = [
                self.io.read(name, port["index"])
                for name in (
                    "Interface 2408 Bank A Format",
                    "Interface 2408 Bank B Format",
                    "Interface 2408 Bank C Format",
                )
            ]
        label_inputs(topology)
        return topology

    def write(
        self,
        control: str,
        bus: int | None,
        input_index: int | None,
        value: Any,
        linked_pairs: list[int] | None = None,
    ) -> dict[str, Any]:
        """Write one control.

        A write to one channel of a linked pair is carried over to its
        partner, and the result then includes both channels' state.
        """
        with self.lock:
            set_control(self.io, control, bus, input_index, value)
            written = read_control(self.io, control, bus, input_index)
            result: dict[str, Any] = {"control": control, "value": written}
            if input_index is None or not control.startswith(("input.", "route.")):
                return result
            left_input = input_index - input_index % 2
            if left_input not in (linked_pairs or []):
                return result
            partner = input_index ^ 1
            try:
                if control in PAIR_SHARED_FIELDS:
                    set_control(self.io, control, bus, partner, written)
            except AlsaError as error:
                raise StereoPairSyncError(
                    f"stereo pair control failed after 1 change: {error}", 1
                ) from error
            result["inputs"] = read_input_states(
                self.io,
                bus,
                (left_input, left_input + 1),
                [left_input],
                self.balance_hints,
            )
            return result

    def sync_stereo_pair(self, left_input: int) -> int:
        with self.lock:
            return sync_input_pair(self.io, left_input)

    def forget_stereo_pair(self, left_input: int) -> None:
        with self.lock:
            for key in [key for key in self.balance_hints if key[1] == left_input]:
                del self.balance_hints[key]

    def write_stereo_pair(
        self,
        control: str,
        bus: int | None,
        left_input: int,
        value: Any,
    ) -> list[dict[str, Any]]:
        with self.lock:
            set_pair_control(
                self.io, control, bus, left_input, value, self.balance_hints
            )
            return read_input_states(
                self.io,
                bus,
                (left_input, left_input + 1),
                [left_input],
                self.balance_hints,
            )

    def snapshot(self) -> dict[str, Any]:
        with self.lock:
            return capture_profile(self.io)

    def apply(self, profile: Any, linked_pairs: list[int] | None = None) -> int:
        with self.lock:
            return apply_profile(self.io, profile, linked_pairs or [])

    def difference(self, profile: Any) -> int:
        with self.lock:
            return profile_difference_count(profile, capture_profile(self.io))


class MeterFeed:
    def __init__(self, device: Device, rate_hz: int = 60) -> None:
        if rate_hz < 1:
            raise ValueError("meter rate must be positive")
        self.device = device
        self.interval = 1 / rate_hz
        self._condition = threading.Condition()
        self._subscribers = 0
        self._sequence = 0
        self._sample: dict[str, Any] | None = None
        self._closed = False
        self._thread = threading.Thread(
            target=self._run, name="qmix-meter-feed", daemon=True
        )
        self._thread.start()

    def subscribe(self) -> int:
        with self._condition:
            if self._closed:
                raise RuntimeError("meter feed is closed")
            self._subscribers += 1
            self._condition.notify_all()
            return self._sequence

    def unsubscribe(self) -> None:
        with self._condition:
            self._subscribers -= 1
            self._condition.notify_all()

    def wait_for_next(
        self, sequence: int, timeout: float
    ) -> tuple[int, dict[str, Any] | None, bool]:
        with self._condition:
            self._condition.wait_for(
                lambda: self._sequence > sequence or self._closed, timeout
            )
            sample = self._sample if self._sequence > sequence else None
            return self._sequence, sample, self._closed

    def close(self) -> None:
        with self._condition:
            self._closed = True
            self._condition.notify_all()
        self._thread.join()

    def _run(self) -> None:
        retry_delay = 1.0
        while True:
            with self._condition:
                self._condition.wait_for(
                    lambda: self._closed or self._subscribers > 0
                )
                if self._closed:
                    return

            started = time.monotonic()
            try:
                sample = self.device.meters()
                deadline = started + self.interval
                retry_delay = 1.0
            except (AlsaError, MixerError, OSError) as error:
                sample = {"error": str(error)}
                deadline = time.monotonic() + retry_delay
                retry_delay = min(retry_delay * 2, 5.0)

            with self._condition:
                if self._closed:
                    return
                self._sequence += 1
                sample["sequence"] = self._sequence
                sample["sampled_at"] = time.monotonic()
                self._sample = sample
                self._condition.notify_all()
                while (
                    not self._closed
                    and self._subscribers > 0
                    and time.monotonic() < deadline
                ):
                    self._condition.wait(deadline - time.monotonic())
                if self._closed:
                    return


class QMixServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(
        self,
        address: tuple[str, int],
        device: Device,
        profiles: ProfileStore,
        channel_names: ChannelNameStore,
        stereo_pairs: StereoPairStore,
        hidden_inputs: HiddenInputStore,
        pairing_code: str,
        allowed_hosts: set[str],
        allowed_origins: set[str] | None = None,
        dev_no_auth: bool = False,
    ) -> None:
        super().__init__(address, QMixHandler)
        self.device = device
        self.profiles = profiles
        self.channel_names = channel_names
        self.stereo_pairs = stereo_pairs
        self.stereo_pair_lock = threading.Lock()
        self.hidden_inputs = hidden_inputs
        self.pairing_code = pairing_code
        self.allowed_hosts = allowed_hosts
        self.allowed_origins = allowed_origins or set()
        self.dev_no_auth = dev_no_auth
        self.dev_csrf = secrets.token_urlsafe(32)
        self.sessions: dict[str, dict[str, Any]] = {}
        self.pair_attempts: dict[str, list[float]] = {}
        self.lock = threading.Lock()
        self.is_tls = False
        self.meter_feed = MeterFeed(device)

    def server_close(self) -> None:
        self.meter_feed.close()
        super().server_close()


class QMixHandler(http.server.BaseHTTPRequestHandler):
    server: QMixServer
    protocol_version = "HTTP/1.1"

    def setup(self) -> None:
        super().setup()
        self.connection.settimeout(10)

    def do_GET(self) -> None:
        if not self._host_allowed():
            return
        parsed = urlsplit(self.path)
        if parsed.path == "/api/session":
            session = self._session()
            self._json(
                200,
                {
                    "authenticated": bool(session),
                    "csrf": session["csrf"] if session else None,
                    "auth_disabled": self.server.dev_no_auth,
                },
            )
            return
        if parsed.path == "/api/meters/stream":
            if self._require_session():
                self._stream_meters()
            return
        if parsed.path.startswith("/api/"):
            if not self._require_session():
                return
            try:
                self._get_api(parsed.path, parse_qs(parsed.query))
            except (MixerError, ProfileStoreError, ValueError) as error:
                self._json(400, {"error": str(error)})
            except StereoPairSyncError as error:
                self._json(503, {"error": str(error)})
            except ChannelNameStoreError as error:
                self._json(503, {"error": str(error)})
            except FlagFileError as error:
                self._json(503, {"error": str(error)})
            except AlsaError as error:
                self._json(503, {"error": str(error)})
            except OSError as error:
                self._json(503, {"error": str(error)})
            return
        self._static(parsed.path)

    def do_POST(self) -> None:
        if not self._host_allowed() or not self._origin_allowed():
            return
        parsed = urlsplit(self.path)
        if parsed.path == "/api/pair":
            if self.server.dev_no_auth:
                self._json(410, {"error": "pairing is disabled in development mode"})
            else:
                self._pair()
            return
        if not self._require_session() or not self._csrf_valid():
            return
        try:
            body = self._body()
            self._post_api(parsed.path, body)
        except (MixerError, ProfileStoreError, ValueError) as error:
            self._json(400, {"error": str(error)})
        except ProfileApplyError as error:
            self._json(409, {"error": str(error), "applied": error.applied})
        except StereoPairSyncError as error:
            self._json(409, {"error": str(error), "applied": error.applied})
        except ChannelNameStoreError as error:
            self._json(503, {"error": str(error)})
        except FlagFileError as error:
            self._json(503, {"error": str(error)})
        except AlsaError as error:
            self._json(503, {"error": str(error)})

    def do_DELETE(self) -> None:
        if not self._host_allowed() or not self._origin_allowed():
            return
        if not self._require_session() or not self._csrf_valid():
            return
        path = urlsplit(self.path).path
        prefix = "/api/profiles/"
        if path.startswith(prefix):
            try:
                self.server.profiles.delete(path[len(prefix):])
                self._json(200, {"deleted": True})
            except ProfileStoreError as error:
                self._json(404, {"error": str(error)})
            return
        self._json(404, {"error": "not found"})

    def do_PUT(self) -> None:
        if not self._host_allowed() or not self._origin_allowed():
            return
        if not self._require_session() or not self._csrf_valid():
            return
        path = urlsplit(self.path).path
        prefix = "/api/profiles/"
        if path.startswith(prefix):
            profile_id = path[len(prefix):]
            try:
                body = self._body()
                if body:
                    raise ValueError("request body must be empty")
                profile = self.server.profiles.replace(
                    profile_id, self.server.device.snapshot()
                )
                self._json(200, profile)
            except ProfileStoreError as error:
                self._json(404, {"error": str(error)})
            except (MixerError, ValueError) as error:
                self._json(400, {"error": str(error)})
            except AlsaError as error:
                self._json(503, {"error": str(error)})
            return
        self._json(404, {"error": "not found"})

    def _get_api(self, path: str, query: dict[str, list[str]]) -> None:
        if path == "/api/status":
            raw_bus = query.get("bus", ["0"])[0]
            try:
                bus = int(raw_bus)
            except ValueError as error:
                raise MixerError("bus must be an integer") from error
            with self.server.stereo_pair_lock:
                status = self.server.device.status(
                    bus, self.server.stereo_pairs.linked_inputs()
                )
            self._json(200, status)
            return
        if path == "/api/channel-names":
            self._json(200, self.server.channel_names.read())
            return
        if path == "/api/stereo-pairs":
            self._json(200, {"pairs": self.server.stereo_pairs.read()})
            return
        if path == "/api/hidden-inputs":
            self._json(200, {"hidden": self.server.hidden_inputs.read()})
            return
        if path == "/api/topology":
            self._json(200, self.server.device.topology())
            return
        if path == "/api/devices":
            self._json(200, self.server.device.devices())
            return
        if path == "/api/profiles":
            self._json(200, {"profiles": self.server.profiles.list()})
            return
        prefix = "/api/profiles/"
        if path.startswith(prefix):
            profile = self.server.profiles.get(path[len(prefix):])
            snapshot = profile.get("snapshot")
            validate_profile(snapshot)
            self._json(
                200,
                {
                    "id": profile["id"],
                    "name": profile["name"],
                    "updated": profile["updated"],
                    "snapshot": snapshot,
                    "changed": self.server.device.difference(snapshot),
                },
            )
            return
        self._json(404, {"error": "not found"})

    def _post_api(self, path: str, body: dict[str, Any]) -> None:
        if path == "/api/devices/control":
            self.server.device.write_device_setting(
                body.get("name"), body.get("index"), body.get("values")
            )
            self._json(200, {"devices": self.server.device.devices()["devices"]})
            return
        if path == "/api/channel-names":
            names = self.server.channel_names.replace(
                body.get("inputs"), body.get("outputs")
            )
            self._json(200, names)
            return
        if path == "/api/stereo-pairs":
            left_input = body.get("left_input")
            enabled = body.get("enabled")
            if not isinstance(enabled, bool):
                raise MixerError("enabled must be a boolean")
            with self.server.stereo_pair_lock:
                was_enabled = self.server.stereo_pairs.is_enabled(left_input)
                copied = 0
                if enabled and not was_enabled:
                    copied = self.server.device.sync_stereo_pair(left_input)
                pairs = self.server.stereo_pairs.set_pair(left_input, enabled)
                if was_enabled and not enabled:
                    self.server.device.forget_stereo_pair(left_input)
            self._json(200, {"pairs": pairs, "copied": copied})
            return
        if path == "/api/hidden-inputs":
            hidden = self.server.hidden_inputs.set_inputs(
                body.get("inputs"), body.get("hidden")
            )
            self._json(200, {"hidden": hidden})
            return
        if path == "/api/stereo-pairs/control":
            left_input = body.get("left_input")
            control = body.get("control")
            bus = body.get("bus")
            if not isinstance(control, str):
                raise MixerError("control must be text")
            if bus is not None and (isinstance(bus, bool) or not isinstance(bus, int)):
                raise MixerError("bus must be an integer")
            with self.server.stereo_pair_lock:
                if not self.server.stereo_pairs.is_enabled(left_input):
                    raise MixerError("stereo pair is not enabled")
                inputs = self.server.device.write_stereo_pair(
                    control,
                    bus,
                    left_input,
                    body.get("value"),
                )
            self._json(
                200,
                {
                    "control": control,
                    "left_input": left_input,
                    "inputs": inputs,
                },
            )
            return
        if path == "/api/control":
            control = body.get("control")
            if not isinstance(control, str):
                raise MixerError("control must be text")
            bus = body.get("bus")
            input_index = body.get("input")
            if bus is not None and (isinstance(bus, bool) or not isinstance(bus, int)):
                raise MixerError("bus must be an integer")
            if input_index is not None and (
                isinstance(input_index, bool) or not isinstance(input_index, int)
            ):
                raise MixerError("input must be an integer")
            with self.server.stereo_pair_lock:
                result = self.server.device.write(
                    control,
                    bus,
                    input_index,
                    body.get("value"),
                    self.server.stereo_pairs.linked_inputs(),
                )
            self._json(200, result)
            return
        if path == "/api/profiles":
            profile = self.server.profiles.create(
                body.get("name"), self.server.device.snapshot()
            )
            self._json(201, profile)
            return
        prefix = "/api/profiles/"
        suffix = "/apply"
        if path.startswith(prefix) and path.endswith(suffix):
            profile_id = path[len(prefix):-len(suffix)]
            profile = self.server.profiles.get(profile_id)
            with self.server.stereo_pair_lock:
                applied = self.server.device.apply(
                    profile.get("snapshot"),
                    self.server.stereo_pairs.linked_inputs(),
                )
            self._json(200, {"applied": applied})
            return
        if path == "/api/logout":
            self._logout()
            return
        self._json(404, {"error": "not found"})

    def _pair(self) -> None:
        address = self.client_address[0]
        now = time.monotonic()
        too_many = False
        with self.server.lock:
            attempts = [
                attempt
                for attempt in self.server.pair_attempts.get(address, [])
                if now - attempt < 300
            ]
            if len(attempts) >= 5:
                self.server.pair_attempts[address] = attempts
                too_many = True
            else:
                self.server.pair_attempts[address] = attempts + [now]
        if too_many:
            self._json(429, {"error": "too many pairing attempts; try again later"})
            return
        try:
            body = self._body()
        except ValueError as error:
            self._json(400, {"error": str(error)})
            return
        supplied = body.get("code")
        if not isinstance(supplied, str) or not hmac.compare_digest(
            supplied.strip(), self.server.pairing_code
        ):
            self._json(403, {"error": "pairing code is incorrect"})
            return
        token = secrets.token_urlsafe(32)
        csrf = secrets.token_urlsafe(32)
        with self.server.lock:
            self.server.sessions[hashlib.sha256(token.encode()).hexdigest()] = {
                "csrf": csrf,
                "expires": time.time() + SESSION_LIFETIME,
            }
            self.server.pair_attempts.pop(address, None)
        secure = "; Secure" if self.server.is_tls else ""
        self._json(
            200,
            {"authenticated": True, "csrf": csrf, "auth_disabled": False},
            {
                "Set-Cookie": (
                    f"{SESSION_COOKIE}={token}; Path=/; HttpOnly; "
                    f"SameSite=Strict; Max-Age={SESSION_LIFETIME}{secure}"
                )
            },
        )

    def _body(self) -> dict[str, Any]:
        raw_length = self.headers.get("Content-Length")
        if raw_length is None:
            raise ValueError("Content-Length is required")
        try:
            length = int(raw_length)
        except ValueError as error:
            raise ValueError("invalid Content-Length") from error
        if not 0 <= length <= MAX_BODY:
            raise ValueError("request body is too large")
        if self.headers.get_content_type() != "application/json":
            raise ValueError("Content-Type must be application/json")
        try:
            value = json.loads(self.rfile.read(length))
        except (json.JSONDecodeError, UnicodeDecodeError) as error:
            raise ValueError("request body is not valid JSON") from error
        if not isinstance(value, dict):
            raise ValueError("request body must be an object")
        return value

    def _session(self) -> dict[str, Any] | None:
        if self.server.dev_no_auth:
            return {"csrf": self.server.dev_csrf}
        cookie = http.cookies.SimpleCookie()
        try:
            cookie.load(self.headers.get("Cookie", ""))
        except http.cookies.CookieError:
            return None
        morsel = cookie.get(SESSION_COOKIE)
        if morsel is None:
            return None
        digest = hashlib.sha256(morsel.value.encode()).hexdigest()
        with self.server.lock:
            session = self.server.sessions.get(digest)
            if session and session["expires"] > time.time():
                return session
            self.server.sessions.pop(digest, None)
        return None

    def _require_session(self) -> bool:
        if self._session():
            return True
        self._json(401, {"error": "authentication required"})
        return False

    def _csrf_valid(self) -> bool:
        session = self._session()
        supplied = self.headers.get("X-Q-Mix-CSRF", "")
        if session and hmac.compare_digest(supplied, session["csrf"]):
            return True
        self._json(403, {"error": "invalid CSRF token"})
        return False

    def _host_allowed(self) -> bool:
        host = self.headers.get("Host", "").lower()
        if host in self.server.allowed_hosts:
            return True
        self._json(421, {"error": "host is not allowed"})
        return False

    def _origin_allowed(self) -> bool:
        origin = self.headers.get("Origin", "")
        host = self.headers.get("Host", "").lower()
        scheme = "https" if self.server.is_tls else "http"
        if origin == f"{scheme}://{host}" or origin in self.server.allowed_origins:
            return True
        self._json(403, {"error": "origin is not allowed"})
        return False

    def _logout(self) -> None:
        cookie = http.cookies.SimpleCookie()
        cookie.load(self.headers.get("Cookie", ""))
        morsel = cookie.get(SESSION_COOKIE)
        if morsel:
            digest = hashlib.sha256(morsel.value.encode()).hexdigest()
            with self.server.lock:
                self.server.sessions.pop(digest, None)
        secure = "; Secure" if self.server.is_tls else ""
        self._json(
            200,
            {"authenticated": False},
            {"Set-Cookie": f"{SESSION_COOKIE}=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0{secure}"},
        )

    def _static(self, path: str) -> None:
        if not WEB_ROOT.is_dir():
            self._json(503, {"error": "web assets are not built; run npm run build"})
            return
        relative = unquote(path).lstrip("/") or "index.html"
        root = WEB_ROOT.resolve()
        target = (root / relative).resolve()
        if not target.is_relative_to(root):
            self._json(404, {"error": "not found"})
            return
        if not target.is_file():
            target = root / "index.html"
        try:
            body = target.read_bytes()
        except OSError:
            self._json(404, {"error": "not found"})
            return
        content_type = mimetypes.guess_type(target.name)[0] or "application/octet-stream"
        self._send(200, body, content_type)

    def _json(self, status: int, data: dict[str, Any], headers: dict[str, str] | None = None) -> None:
        self._send(
            status,
            json.dumps(data, separators=(",", ":"), ensure_ascii=True).encode(),
            "application/json; charset=utf-8",
            headers,
        )

    def _send(
        self,
        status: int,
        body: bytes,
        content_type: str,
        headers: dict[str, str] | None = None,
    ) -> None:
        self._response_headers(status, content_type, headers, len(body))
        self.wfile.write(body)

    def _response_headers(
        self,
        status: int,
        content_type: str,
        headers: dict[str, str] | None = None,
        content_length: int | None = None,
    ) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        if content_length is not None:
            self.send_header("Content-Length", str(content_length))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        if self.server.is_tls:
            self.send_header("Strict-Transport-Security", "max-age=31536000")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
            "connect-src 'self'; img-src 'self' data:; object-src 'none'; "
            "base-uri 'self'; frame-ancestors 'none'",
        )
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()

    def _stream_meters(self) -> None:
        feed = self.server.meter_feed
        try:
            sequence = feed.subscribe()
        except RuntimeError as error:
            self._json(503, {"error": str(error)})
            return
        self.close_connection = True
        try:
            self._response_headers(
                200,
                "text/event-stream; charset=utf-8",
                {
                    "Connection": "close",
                    "Transfer-Encoding": "chunked",
                    "X-Accel-Buffering": "no",
                },
            )
            while True:
                next_sequence, sample, closed = feed.wait_for_next(sequence, 5.0)
                if closed:
                    break
                if sample is None:
                    self._write_chunk(b": keepalive\n\n")
                else:
                    sequence = next_sequence
                    payload = json.dumps(sample, separators=(",", ":"), ensure_ascii=True)
                    self._write_chunk(
                        f"id: {sequence}\nevent: meters\ndata: {payload}\n\n".encode()
                    )
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            pass
        finally:
            feed.unsubscribe()

    def _write_chunk(self, body: bytes) -> None:
        self.wfile.write(f"{len(body):X}\r\n".encode())
        self.wfile.write(body)
        self.wfile.write(b"\r\n")
        self.wfile.flush()

    def log_message(self, format_string: str, *args: Any) -> None:
        print(f"{self.client_address[0]} - {format_string % args}")


def _host_port(host: str, port: int) -> str:
    if ":" in host and not host.startswith("["):
        host = f"[{host}]"
    return f"{host.lower()}:{port}"


def _validate_listener(
    host: str,
) -> tuple[ipaddress.IPv4Address | ipaddress.IPv6Address, bool]:
    try:
        address = ipaddress.ip_address(host)
    except ValueError as error:
        raise ValueError("listen address must be an explicit IPv4 or IPv6 address") from error
    if address.is_unspecified or address.is_multicast:
        raise ValueError("wildcard and multicast listen addresses are not allowed")
    if not (address.is_loopback or address.is_private or address.is_link_local):
        raise ValueError("only loopback or private/link-local LAN addresses are allowed")
    return address, address.is_loopback


def main() -> None:
    parser = argparse.ArgumentParser(description="Serve the Q-Mix web UI for MOTU PCI-424")
    parser.add_argument("--card", default="M424", help="ALSA card id or control name (default: M424)")
    parser.add_argument("--listen", default="127.0.0.1", help="loopback or private LAN IP")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--allowed-host", action="append", default=[], help="additional Host header name")
    parser.add_argument("--tls-cert", type=Path)
    parser.add_argument("--tls-key", type=Path)
    parser.add_argument(
        "--dev-origin",
        action="store_true",
        help="allow the loopback Vite dev server origin; loopback only",
    )
    parser.add_argument(
        "--dev-no-auth",
        action="store_true",
        help="disable pairing and authentication for development; any allowed LAN client can control the mixer",
    )
    parser.add_argument(
        "--profiles-dir",
        type=Path,
        default=Path.home() / ".local/share/motu424-qmix/profiles",
    )
    args = parser.parse_args()
    try:
        address, loopback = _validate_listener(args.listen)
    except ValueError as error:
        parser.error(str(error))
    if not 1 <= args.port <= 65535:
        parser.error("port must be between 1 and 65535")
    if bool(args.tls_cert) != bool(args.tls_key):
        parser.error("--tls-cert and --tls-key must be provided together")
    if not loopback and not args.tls_cert:
        parser.error("LAN access requires --tls-cert and --tls-key")
    if args.dev_origin and not loopback:
        parser.error("--dev-origin is only allowed with a loopback listener")
    try:
        device = Device(args.card)
        profiles = ProfileStore(args.profiles_dir)
        channel_names = ChannelNameStore(
            Path.home() / ".local/share/motu424-qmix/channel-names.json"
        )
        stereo_pairs = StereoPairStore(
            Path.home() / ".local/share/motu424-qmix/stereo-pairs.json"
        )
        hidden_inputs = HiddenInputStore(
            Path.home() / ".local/share/motu424-qmix/hidden-inputs.json"
        )
    except (AlsaError, ChannelNameStoreError, FlagFileError, OSError) as error:
        parser.error(str(error))
    for left_input in stereo_pairs.linked_inputs():
        try:
            device.sync_stereo_pair(left_input)
        except (AlsaError, StereoPairSyncError) as error:
            print(f"WARNING: stereo pair {left_input + 1}/{left_input + 2}: {error}")
    pairing_code = "" if args.dev_no_auth else secrets.token_urlsafe(10)
    allowed_hosts = {
        _host_port(str(address), args.port),
        _host_port("localhost", args.port),
        _host_port("127.0.0.1", args.port),
        *(_host_port(host, args.port) for host in args.allowed_host),
    }
    allowed_origins = (
        {"http://127.0.0.1:5173", "http://localhost:5173"}
        if args.dev_origin
        else set()
    )
    server = QMixServer(
        (str(address), args.port),
        device,
        profiles,
        channel_names,
        stereo_pairs,
        hidden_inputs,
        pairing_code,
        allowed_hosts,
        allowed_origins,
        dev_no_auth=args.dev_no_auth,
    )
    if args.tls_cert:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(args.tls_cert, args.tls_key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        server.is_tls = True
    scheme = "https" if server.is_tls else "http"
    print(f"Q-Mix UI: {scheme}://{address}:{args.port}/")
    if args.dev_no_auth:
        print(
            "WARNING: development mode disables pairing; "
            "any client on the allowed network can control the mixer."
        )
    else:
        print(f"Pairing code: {pairing_code}")
    if args.allowed_host:
        print("Additional hostnames: " + ", ".join(args.allowed_host))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        device.close()


if __name__ == "__main__":
    main()
