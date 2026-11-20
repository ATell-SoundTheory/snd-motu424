import json
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from http.cookiejar import CookieJar
from pathlib import Path
from unittest.mock import patch

from qmixui.alsa import AlsaError
from qmixui.channel_names import ChannelNameStore
from qmixui.hidden_inputs import HiddenInputStore
from qmixui.profiles import ProfileStore
from qmixui.server import QMixServer, Device, MeterFeed, _validate_listener
from qmixui.stereo_pairs import StereoPairStore
from test_mixer import FakeControl


class FakeDevice:
    def __init__(self) -> None:
        self.writes = []
        self.meter_reads = 0
        self.meter_lock = threading.Lock()

    def status(self, bus: int, linked_pairs: list[int] | None = None) -> dict:
        return {"bus": {"index": bus}}

    def meters(self) -> dict:
        with self.meter_lock:
            self.meter_reads += 1
        return {"input_bank": 0, "bus": 0, "values": [123] * 52}

    def topology(self) -> dict:
        return {"known": False, "inputs": [None] * 96, "outputs": [None] * 48, "ports": []}

    def write(
        self,
        control: str,
        bus: int | None,
        input_index: int | None,
        value: int,
        linked_pairs: list[int] | None = None,
    ) -> dict:
        self.writes.append((control, bus, input_index, value))
        return {"control": control, "value": value}

    def snapshot(self) -> dict:
        return {"snapshot": True}

    def difference(self, _profile: dict) -> int:
        return 0

    def apply(self, _profile: dict) -> int:
        return 0

    def close(self) -> None:
        pass


class ServerSecurityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        server = QMixServer(
            ("127.0.0.1", 0),
            FakeDevice(),
            ProfileStore(Path(self.temporary.name) / "profiles"),
            ChannelNameStore(Path(self.temporary.name) / "channel-names.json"),
            StereoPairStore(Path(self.temporary.name) / "stereo-pairs.json"),
            HiddenInputStore(Path(self.temporary.name) / "hidden-inputs.json"),
            "pair-code",
            set(),
        )
        host, port = server.server_address
        server.allowed_hosts = {f"{host}:{port}"}
        self.server = server
        self.origin = f"http://{host}:{port}"
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(CookieJar())
        )
        self.thread = threading.Thread(target=server.serve_forever, daemon=True)
        self.thread.start()
        self.base = self.origin
        self.csrf = ""

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.temporary.cleanup()

    def request(self, path: str, method: str = "GET", data: dict | None = None, **headers):
        body = json.dumps(data).encode() if data is not None else None
        request = urllib.request.Request(
            self.base + path,
            data=body,
            method=method,
            headers=headers,
        )
        return self.opener.open(request)

    def pair(self) -> None:
        response = self.request(
            "/api/pair",
            "POST",
            {"code": "pair-code"},
            Origin=self.origin,
            **{"Content-Type": "application/json"},
        )
        self.csrf = json.loads(response.read())["csrf"]

    def test_host_allowlist_blocks_unknown_host(self) -> None:
        with self.assertRaises(urllib.error.HTTPError) as error:
            self.request("/api/session", Host="attacker.example")
        self.assertEqual(error.exception.code, 421)
        error.exception.close()

    def test_listener_rejects_wildcard_and_public_addresses(self) -> None:
        with self.assertRaises(ValueError):
            _validate_listener("0.0.0.0")
        with self.assertRaises(ValueError):
            _validate_listener("8.8.8.8")
        self.assertTrue(_validate_listener("192.168.1.20")[0].is_private)

    def test_pairing_authenticates_device_and_mutations_require_origin_and_csrf(self) -> None:
        with self.assertRaises(urllib.error.HTTPError) as error:
            self.request("/api/profiles")
        self.assertEqual(error.exception.code, 401)
        error.exception.close()

        with self.assertRaises(urllib.error.HTTPError) as error:
            self.request(
                "/api/pair",
                "POST",
                {"code": "pair-code"},
                Origin="https://attacker.example",
                **{"Content-Type": "application/json"},
            )
        self.assertEqual(error.exception.code, 403)
        error.exception.close()

        self.pair()
        session = self.request("/api/session")
        self.assertTrue(json.loads(session.read())["authenticated"])

        with self.assertRaises(urllib.error.HTTPError) as error:
            self.request(
                "/api/control",
                "POST",
                {"control": "route.volume", "bus": 0, "input": 1, "value": 2},
                Origin="https://attacker.example",
                **{
                    "Content-Type": "application/json",
                    "X-Q-Mix-CSRF": self.csrf,
                },
            )
        self.assertEqual(error.exception.code, 403)
        error.exception.close()

        with self.assertRaises(urllib.error.HTTPError) as error:
            self.request(
                "/api/control",
                "POST",
                {"control": "route.volume", "bus": 0, "input": 1, "value": 2},
                Origin=self.origin,
                **{"Content-Type": "application/json"},
            )
        self.assertEqual(error.exception.code, 403)
        error.exception.close()

        response = self.request(
            "/api/control",
            "POST",
            {"control": "route.volume", "bus": 0, "input": 1, "value": 2},
            Origin=self.origin,
            **{
                "Content-Type": "application/json",
                "X-Q-Mix-CSRF": self.csrf,
            },
        )
        self.assertEqual(response.status, 200)
        self.assertEqual(
            self.server.device.writes,
            [("route.volume", 0, 1, 2)],
        )

    def test_development_mode_skips_pairing_but_keeps_origin_and_csrf_checks(self) -> None:
        self.server.dev_no_auth = True
        session = json.loads(self.request("/api/session").read())
        self.assertTrue(session["authenticated"])
        self.assertTrue(session["auth_disabled"])
        self.assertTrue(session["csrf"])

        with self.assertRaises(urllib.error.HTTPError) as error:
            self.request(
                "/api/pair",
                "POST",
                {"code": "pair-code"},
                Origin=self.origin,
                **{"Content-Type": "application/json"},
            )
        self.assertEqual(error.exception.code, 410)
        error.exception.close()

        with self.assertRaises(urllib.error.HTTPError) as error:
            self.request(
                "/api/control",
                "POST",
                {"control": "route.volume", "bus": 0, "input": 1, "value": 2},
                Origin="https://attacker.example",
                **{
                    "Content-Type": "application/json",
                    "X-Q-Mix-CSRF": session["csrf"],
                },
            )
        self.assertEqual(error.exception.code, 403)
        error.exception.close()

        with self.assertRaises(urllib.error.HTTPError) as error:
            self.request(
                "/api/control",
                "POST",
                {"control": "route.volume", "bus": 0, "input": 1, "value": 2},
                Origin=self.origin,
                **{"Content-Type": "application/json"},
            )
        self.assertEqual(error.exception.code, 403)
        error.exception.close()

        response = self.request(
            "/api/control",
            "POST",
            {"control": "route.volume", "bus": 0, "input": 1, "value": 2},
            Origin=self.origin,
            **{
                "Content-Type": "application/json",
                "X-Q-Mix-CSRF": session["csrf"],
            },
        )
        self.assertEqual(response.status, 200)

    def test_meter_stream_requires_authentication_and_sends_samples(self) -> None:
        with self.assertRaises(urllib.error.HTTPError) as error:
            self.request("/api/meters/stream")
        self.assertEqual(error.exception.code, 401)
        error.exception.close()

        self.pair()
        response = self.request("/api/meters/stream")
        try:
            self.assertEqual(
                response.headers.get_content_type(),
                "text/event-stream",
            )
            lines = []
            while True:
                line = response.readline()
                lines.append(line)
                if line in (b"\n", b""):
                    break
            payload_line = next(line for line in lines if line.startswith(b"data: "))
            sample = json.loads(payload_line.removeprefix(b"data: "))
            self.assertEqual(sample["input_bank"], 0)
            self.assertEqual(sample["values"], [123] * 52)
            self.assertEqual(sample["sequence"], 1)
        finally:
            response.close()

    def test_channel_names_api_saves_server_side_names(self) -> None:
        with self.assertRaises(urllib.error.HTTPError) as error:
            self.request("/api/channel-names")
        self.assertEqual(error.exception.code, 401)
        error.exception.close()

        self.pair()
        empty = self.request("/api/channel-names")
        self.assertEqual(json.loads(empty.read())["inputs"], [""] * 96)

        names = {
            "inputs": ["  Vocal mic  "] + [""] * 95,
            "outputs": ["Monitors"] + [""] * 47,
        }
        response = self.request(
            "/api/channel-names",
            "POST",
            names,
            Origin=self.origin,
            **{
                "Content-Type": "application/json",
                "X-Q-Mix-CSRF": self.csrf,
            },
        )
        saved = json.loads(response.read())
        self.assertEqual(saved["inputs"][0], "Vocal mic")
        self.assertEqual(saved["inputs"][1], "")
        self.assertEqual(saved["outputs"][0], "Monitors")
        self.assertEqual(
            json.loads(self.request("/api/channel-names").read()),
            saved,
        )

    def test_stereo_pair_api_keeps_linked_channels_consistent(self) -> None:
        io = FakeControl()
        self.server.device = Device("test", io)
        volume = "Q-Mix Bus 7 Input Playback Volume"
        pan = "Q-Mix Bus 7 Input Playback Pan"
        mute = "Q-Mix Bus 7 Input Playback Switch"
        io.values.update({(volume, 2): 30000, (volume, 3): 15000, (pan, 2): 20})
        self.pair()
        headers = {
            "Origin": self.origin,
            "Content-Type": "application/json",
            "X-Q-Mix-CSRF": self.csrf,
        }

        def post(path: str, data: dict) -> dict:
            return json.loads(self.request(path, "POST", data, **headers).read())

        linked = post("/api/stereo-pairs", {"left_input": 2, "enabled": True})
        self.assertTrue(linked["pairs"][1])
        self.assertEqual((io.read(volume, 2), io.read(volume, 3)), (30000, 15000))
        self.assertEqual((io.read(pan, 2), io.read(pan, 3)), (20, 64))

        status = json.loads(self.request("/api/status?bus=7").read())
        stereo = status["inputs"][2]["stereo"]
        self.assertEqual((stereo["gain"], stereo["width"], stereo["pan"]), (30000, 86, 42))
        self.assertIsNone(status["inputs"][3]["stereo"])
        self.assertIsNone(status["inputs"][4]["stereo"])

        result = post(
            "/api/stereo-pairs/control",
            {"left_input": 2, "control": "pair.gain", "bus": 7, "value": 20000},
        )
        self.assertEqual(
            [state["route"]["volume"] for state in result["inputs"]], [20000, 10000]
        )
        self.assertEqual(result["inputs"][0]["stereo"]["balance"], stereo["balance"])

        result = post(
            "/api/stereo-pairs/control",
            {"left_input": 2, "control": "pair.pan", "bus": 7, "value": 54},
        )
        self.assertEqual([state["route"]["pan"] for state in result["inputs"]], [32, 76])
        post("/api/control", {"control": "route.pan", "bus": 7, "input": 3, "value": 77})
        self.assertEqual((io.read(pan, 2), io.read(pan, 3)), (32, 77))

        result = post(
            "/api/control",
            {"control": "route.enabled", "bus": 7, "input": 3, "value": 1},
        )
        self.assertEqual((io.read(mute, 2), io.read(mute, 3)), (1, 1))
        self.assertEqual([state["index"] for state in result["inputs"]], [2, 3])

        io.values[(mute, 3)] = 0
        self.request("/api/status?bus=7").read()
        self.assertEqual(io.read(mute, 3), 1)

        unlinked = post("/api/stereo-pairs", {"left_input": 2, "enabled": False})
        self.assertFalse(unlinked["pairs"][1])
        self.assertEqual((io.read(volume, 2), io.read(volume, 3)), (20000, 10000))
        with self.assertRaises(urllib.error.HTTPError) as error:
            self.request(
                "/api/stereo-pairs/control",
                "POST",
                {"left_input": 2, "control": "pair.gain", "bus": 7, "value": 1},
                **headers,
            )
        self.assertEqual(error.exception.code, 400)

    def test_device_settings_discovery_writes_and_validation(self) -> None:
        io = FakeControl()
        def setting(name, index=0, values=None, maximum=3, writable=True):
            return {
                "name": name, "index": index, "kind": "integer",
                "writable": writable, "values": values or [0],
                "min": 0, "max": maximum, "step": 1, "options": [],
            }
        io.settings = [
            setting("Card Clock Source", maximum=11),
            setting("SMPTE Generator Start Time", values=[1, 0, 0, 0], maximum=59),
            setting("SMPTE Reader State", maximum=6, writable=False),
            setting("Interface 2408 Analog Bank", index=1),
            setting("Interface 2408 Analog Bank", index=3),
            setting("Interface AES Output SRC Mode", index=1, maximum=13),
            setting("SMPTE Generator Audio Overwritten", values=[1], maximum=1),
        ]
        topology = {
            "ports": [
                {"index": 0, "name": "24i", "configured": True},
                {"index": 1, "name": "2408", "configured": True},
                {"index": 2, "name": "Port 2", "configured": False},
                {"index": 3, "name": "2408", "configured": True},
            ],
            "inputs": [True] * 96, "outputs": [True] * 48,
        }
        device = Device("test", io)
        self.server.device = device
        self.pair()
        headers = {
            "Origin": self.origin, "Content-Type": "application/json",
            "X-Q-Mix-CSRF": self.csrf,
        }
        with patch.object(device, "_read_topology", return_value=topology):
            devices = json.loads(self.request("/api/devices").read())["devices"]
            self.assertEqual([device["port"] for device in devices], [None, 0, 1, 3])
            self.assertEqual(devices[1]["controls"], [])
            self.assertEqual(devices[2]["controls"][0]["options"][3]["label"], "None")
            for name, index, values in (
                ("Interface 2408 Analog Bank", 3, [2]),
                ("SMPTE Generator Start Time", 0, [2, 3, 4, 5]),
                ("SMPTE Generator Audio Overwritten", 0, [0]),
            ):
                response = self.request(
                    "/api/devices/control", "POST",
                    {"name": name, "index": index, "values": values}, **headers,
                )
                self.assertEqual(response.status, 200)
                response.close()
                self.assertEqual(
                    next(control for control in io.settings if
                         (control["name"], control["index"]) == (name, index))["values"],
                    values,
                )
            for name, index, values in (
                ("Card Clock Source", 0, [1]),
                ("Interface 2408 Analog Bank", 1, [4]),
                ("Interface 2408 Analog Bank", 1, [1.5]),
                ("Interface 2408 Analog Bank", 1, [True]),
                ("Interface 2408 Analog Bank", 0, [1]),
                ("Interface AES Output SRC Mode", 1, [6]),
                ("SMPTE Reader State", 0, [1]),
                ("SMPTE Generator Audio Overwritten", 0, [1]),
                ("SMPTE Generator Start Time", 0, [1]),
                ("Q-Mix Bus Playback Volume", 0, [1]),
            ):
                with self.subTest(name=name, values=values):
                    with self.assertRaises(urllib.error.HTTPError) as error:
                        self.request(
                            "/api/devices/control", "POST",
                            {"name": name, "index": index, "values": values}, **headers,
                        )
                    self.assertEqual(error.exception.code, 400)
                    error.exception.close()
            self.assertEqual(io.settings[3]["values"], [0])

    def test_hidden_inputs_api_saves_server_side_flags(self) -> None:
        self.pair()
        headers = {
            "Origin": self.origin,
            "Content-Type": "application/json",
            "X-Q-Mix-CSRF": self.csrf,
        }
        saved = json.loads(
            self.request(
                "/api/hidden-inputs", "POST", {"inputs": [4, 5], "hidden": True}, **headers
            ).read()
        )
        self.assertEqual(
            [index for index, hidden in enumerate(saved["hidden"]) if hidden], [4, 5]
        )
        self.assertEqual(json.loads(self.request("/api/hidden-inputs").read()), saved)
        with self.assertRaises(urllib.error.HTTPError) as error:
            self.request(
                "/api/hidden-inputs", "POST", {"inputs": [96], "hidden": True}, **headers
            )
        self.assertEqual(error.exception.code, 400)


class MeterFeedTests(unittest.TestCase):
    def test_subscribers_share_one_meter_reader(self) -> None:
        device = FakeDevice()
        feed = MeterFeed(device, rate_hz=1)
        first_sequence = feed.subscribe()
        second_sequence = feed.subscribe()
        try:
            sequence, first_sample, closed = feed.wait_for_next(first_sequence, 2)
            second_frame = feed.wait_for_next(second_sequence, 0)
            self.assertFalse(closed)
            self.assertEqual(sequence, 1)
            self.assertEqual(second_frame[0], sequence)
            self.assertEqual(first_sample, second_frame[1])
            self.assertEqual(device.meter_reads, 1)
        finally:
            feed.unsubscribe()
            feed.unsubscribe()
            feed.close()

    def test_reader_errors_are_published(self) -> None:
        class FailingDevice(FakeDevice):
            def meters(self) -> dict:
                raise AlsaError("DSP meter read failed")

        feed = MeterFeed(FailingDevice())
        sequence = feed.subscribe()
        try:
            next_sequence, sample, closed = feed.wait_for_next(sequence, 1)
            self.assertFalse(closed)
            self.assertEqual(next_sequence, sequence + 1)
            self.assertEqual(sample["error"], "DSP meter read failed")
        finally:
            feed.unsubscribe()
            feed.close()


if __name__ == "__main__":
    unittest.main()
