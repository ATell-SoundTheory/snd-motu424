import tempfile
import unittest
from copy import deepcopy
from pathlib import Path

from qmixui.alsa import AlsaError
from qmixui.mixer import (
    MixerError,
    ProfileApplyError,
    StereoPairSyncError,
    apply_profile,
    capture_profile,
    input_label,
    input_source_label,
    read_bus_state,
    read_input_states,
    read_topology,
    set_control,
    set_pair_control,
    sync_stereo_pair,
    validate_profile,
)
from qmixui.profiles import ProfileStore


class FakeControl:
    def __init__(self) -> None:
        self.values: dict[tuple[str, int], int] = {}
        self.writes: list[tuple[str, int, int, bool]] = []
        self.fail_on: tuple[str, int] | None = None
        self.settings: list[dict] = []

    def close(self) -> None:
        pass

    def hardware_controls(self) -> list[dict]:
        return deepcopy(self.settings)

    def write_hardware_control(
        self, name: str, index: int, values: list[int], kind: str
    ) -> None:
        for setting in self.settings:
            if (setting["name"], setting["index"]) == (name, index):
                setting["values"] = values[:]
                return
        raise AlsaError("unknown hardware setting")

    def _default(self, name: str) -> int:
        if name == "Q-Mix Input Trim Volume":
            return 64
        if name == "Q-Mix Input Switch":
            return 1
        if name.endswith("Playback Volume"):
            return 32768
        if name.endswith("Playback Pan") or name.endswith("Stereo Balance") or name.endswith("Stereo Width"):
            return 64
        if name.endswith("Talkback Input") or name.endswith("Listenback Input"):
            return 96
        if name.endswith("Talkback Dim") or name.endswith("Listenback Dim"):
            return 128
        if name == "Q-Mix Meter Bus":
            return 48
        return 0

    def read(self, name: str, index: int, boolean: bool = False) -> int:
        del boolean
        return self.values.get((name, index), self._default(name))

    def read_members(self, name: str, count: int) -> list[int]:
        del name
        return [0] * count

    def write(self, name: str, index: int, value: int, boolean: bool = False) -> None:
        if self.fail_on == (name, index):
            raise AlsaError("simulated ALSA write failure")
        self.values[(name, index)] = value
        self.writes.append((name, index, value, boolean))


class MixerTests(unittest.TestCase):
    def test_control_mapping_and_bounds(self) -> None:
        io = FakeControl()
        self.assertEqual(set_control(io, "route.pan", 47, 95, 0), 0)
        self.assertEqual(
            io.writes[-1],
            ("Q-Mix Bus 47 Input Playback Pan", 95, 0, False),
        )
        with self.assertRaises(MixerError):
            set_control(io, "route.volume", 48, 0, 100)
        with self.assertRaises(MixerError):
            set_control(io, "input.trim", None, 96, 64)
        with self.assertRaises(MixerError):
            set_control(io, "stereo.width", 0, 3, 64)
        self.assertEqual(len(io.writes), 1)

    def test_proc_topology_maps_physical_input_and_output_pairs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "cards").write_text(" 0 [M424           ]: MOTU - PCI-424\n")
            card = root / "card0"
            card.mkdir()
            (card / "motu424").write_text(
                "MOTU PCI-424X/PCIe-424 at 0x50400000, irq 16\n"
                "Port 0: 24I/O, configured (0 MIDI bytes received)\n"
                "Port 1: none, not configured (0 MIDI bytes received)\n"
                "Port 2: not connected (0 MIDI bytes received)\n"
                "Capture: 2 channels\n"
                "  1-2: port 0, 24I/O 1-2\n"
                "Playback: 2 channels\n"
                "  1-2: port 0, 24I/O 1-2\n"
            )
            topology = read_topology("M424", root)
        self.assertTrue(topology["known"])
        self.assertEqual(topology["card_name"], "MOTU PCI-424X/PCIe-424")
        self.assertEqual(topology["ports"][2]["name"], "Port 2")
        self.assertEqual(topology["ports"][0]["name"], "24I/O")
        self.assertTrue(topology["ports"][0]["configured"])
        self.assertTrue(topology["inputs"][0])
        self.assertFalse(topology["inputs"][2])
        self.assertTrue(topology["outputs"][0])
        self.assertFalse(topology["outputs"][1])
        self.assertEqual(topology["input_labels"][0], "Port 01 · Analog 1")

    def test_input_labels_include_2408_analog_and_optical_bank_names(self) -> None:
        topology = {
            "ports": [
                {"index": 0, "name": "Port 0"},
                {
                    "index": 1,
                    "name": "2408",
                    "analog_bank": 0,
                    "bank_formats": [0, 0, 0],
                },
                {"index": 2, "name": "Port 2"},
                {"index": 3, "name": "Port 3"},
            ],
        }
        self.assertEqual(input_label(topology, 24), "Port 02 · Analog 1")
        self.assertEqual(input_label(topology, 37), "Port 02 · Optical A6")
        self.assertEqual(input_label(topology, 47), "Port 02 · Optical B8")
        self.assertEqual(input_source_label(topology, 37), "Optical A6")

    def test_status_uses_fixed_physical_indices(self) -> None:
        io = FakeControl()
        topology = {
            "known": False,
            "ports": [
                {"index": port, "name": f"Port {port}", "configured": None}
                for port in range(4)
            ],
            "inputs": [None] * 96,
            "outputs": [None] * 48,
        }
        status = read_bus_state(io, 12, topology)
        self.assertEqual(status["bus"]["index"], 12)
        self.assertEqual(status["bus"]["label"], "Port 02 · Port 1 · Out 01")
        self.assertEqual(status["inputs"][24]["index"], 24)
        self.assertIn("Port 02", status["inputs"][24]["label"])
        self.assertIsNone(status["inputs"][24]["stereo"])

    def test_stereo_pair_sync_keeps_levels_and_arbitrary_pans(self) -> None:
        io = FakeControl()
        io.values[("Q-Mix Input Trim Volume", 0)] = 128
        io.values[("Q-Mix Input Switch", 0)] = 0
        for bus in range(48):
            for name, left, right in (
                ("Input Playback Volume", 12000 + bus, 30000 - bus),
                ("Input Playback Pan", 10, 100),
                ("Input Playback Switch", bus % 2, 1 - bus % 2),
                ("Input Solo Switch", 1, 0),
            ):
                control = f"Q-Mix Bus {bus} {name}"
                io.values[(control, 0)] = left
                io.values[(control, 1)] = right

        sync_stereo_pair(io, 0)

        self.assertEqual(io.values[("Q-Mix Input Trim Volume", 1)], 128)
        self.assertEqual(io.values[("Q-Mix Input Switch", 1)], 0)
        for bus in range(48):
            prefix = f"Q-Mix Bus {bus} Input "
            self.assertEqual(io.values[(prefix + "Playback Volume", 0)], 12000 + bus)
            self.assertEqual(io.values[(prefix + "Playback Volume", 1)], 30000 - bus)
            self.assertEqual(io.values[(prefix + "Playback Pan", 0)], 10)
            self.assertEqual(io.values[(prefix + "Playback Pan", 1)], 100)
            for name in ("Playback Switch", "Solo Switch"):
                self.assertEqual(
                    io.values[(prefix + name, 1)], io.values[(prefix + name, 0)]
                )
        self.assertEqual(sync_stereo_pair(io, 0), 0)

    def test_pair_controls_and_routes_convert_both_ways(self) -> None:
        io = FakeControl()
        hints: dict[tuple[int, int], float] = {}
        volume = "Q-Mix Bus 3 Input Playback Volume"
        pan = "Q-Mix Bus 3 Input Playback Pan"
        io.values.update(
            {(volume, 4): 20000, (volume, 5): 12000, (pan, 4): 0, (pan, 5): 128}
        )

        def stereo() -> dict:
            return read_input_states(io, 3, (4, 5), [4], hints)[0]["stereo"]

        unlinked = stereo()
        self.assertEqual(unlinked["gain"], 20000)
        self.assertLess(unlinked["balance"], 64)
        self.assertEqual(unlinked["width"], 128)
        self.assertEqual(unlinked["pan"], 64)

        set_pair_control(io, "pair.gain", 3, 4, 10000, hints)
        self.assertEqual((io.read(volume, 4), io.read(volume, 5)), (10000, 6000))
        self.assertEqual(stereo()["balance"], unlinked["balance"])

        set_pair_control(io, "pair.gain", 3, 4, 0, hints)
        self.assertEqual((io.read(volume, 4), io.read(volume, 5)), (0, 0))
        set_pair_control(io, "pair.gain", 3, 4, 20000, hints)
        self.assertEqual((io.read(volume, 4), io.read(volume, 5)), (20000, 12000))

        set_pair_control(io, "pair.width", 3, 4, 40, hints)
        self.assertEqual((io.read(pan, 4), io.read(pan, 5)), (88, 40))
        self.assertEqual((io.read(volume, 4), io.read(volume, 5)), (20000, 12000))

        set_pair_control(io, "pair.balance", 3, 4, 96, hints)
        self.assertEqual((io.read(volume, 4), io.read(volume, 5)), (15000, 20000))
        self.assertEqual(stereo(), {"gain": 20000, "balance": 96, "width": 40, "pan": 64})

        io.values[(volume, 4)] = 20000
        self.assertEqual(stereo()["balance"], 64)

    def test_pair_controls_reject_invalid_values(self) -> None:
        io = FakeControl()
        for field, value in (
            ("pair.gain", 32769),
            ("pair.balance", 128.5),
            ("pair.balance", float("nan")),
            ("pair.width", 1.25),
            ("pair.pan", 129),
            ("route.volume", 100),
        ):
            with self.subTest(field=field, value=value), self.assertRaises(MixerError):
                set_pair_control(io, field, 0, 0, value, {})
        self.assertEqual(io.writes, [])

    def test_stereo_pair_sync_reports_partial_write_failure(self) -> None:
        io = FakeControl()
        io.values[("Q-Mix Bus 0 Input Solo Switch", 0)] = 1
        io.fail_on = ("Q-Mix Bus 0 Input Solo Switch", 1)

        with self.assertRaises(StereoPairSyncError) as error:
            sync_stereo_pair(io, 0)

        self.assertEqual(error.exception.applied, 0)

    def test_stereo_pair_sync_requires_left_channel_index(self) -> None:
        with self.assertRaises(MixerError):
            sync_stereo_pair(FakeControl(), 1)

    def test_profile_application_orders_stereo_before_mono_and_enables_last(self) -> None:
        io = FakeControl()
        profile = capture_profile(io)
        profile["buses"][0]["enabled"] = True
        profile["buses"][0]["stereo"][0] = {"balance": 32, "width": 80}
        profile["buses"][0]["routes"][0]["volume"] = 12000
        profile["buses"][0]["routes"][0]["pan"] = 20
        validate_profile(profile)

        updated = apply_profile(io, profile)

        stereo_write = next(i for i, write in enumerate(io.writes) if write[0].endswith("Stereo Balance"))
        route_volume_write = next(
            i for i, write in enumerate(io.writes)
            if write[0] == "Q-Mix Bus 0 Input Playback Volume" and write[1] == 0
        )
        bus_enable_write = max(
            i for i, write in enumerate(io.writes)
            if write[0] == "Q-Mix Bus Playback Switch" and write[1] == 0
        )
        self.assertLess(stereo_write, route_volume_write)
        self.assertEqual(io.writes[route_volume_write][2], 12000)
        self.assertEqual(io.writes[-1][:3], ("Q-Mix Bus Playback Switch", 0, 1))
        self.assertEqual(updated, len(io.writes))

    def test_profile_application_restores_linked_stereo_pair_settings(self) -> None:
        io = FakeControl()
        profile = capture_profile(io)
        profile["inputs"][0]["trim"] = 96
        profile["inputs"][0]["enabled"] = False
        profile["buses"][0]["enabled"] = True
        profile["buses"][0]["routes"][0].update(
            {"volume": 12000, "pan": 32, "enabled": True, "solo": True}
        )

        apply_profile(io, profile, linked_pairs=[0])

        self.assertEqual(
            io.values[("Q-Mix Input Trim Volume", 1)],
            io.values[("Q-Mix Input Trim Volume", 0)],
        )
        self.assertEqual(
            io.values[("Q-Mix Input Switch", 1)],
            io.values[("Q-Mix Input Switch", 0)],
        )
        for bus in range(48):
            for name in ("Input Playback Switch", "Input Solo Switch"):
                control = f"Q-Mix Bus {bus} {name}"
                self.assertEqual(io.read(control, 1), io.read(control, 0))
        self.assertEqual(io.read("Q-Mix Bus 0 Input Playback Volume", 0), 12000)
        self.assertEqual(io.read("Q-Mix Bus 0 Input Playback Volume", 1), 32768)
        self.assertEqual(io.read("Q-Mix Bus 0 Input Playback Pan", 0), 32)
        self.assertEqual(io.read("Q-Mix Bus 0 Input Playback Pan", 1), 64)
        sync_write = max(
            index for index, write in enumerate(io.writes)
            if write[0] == "Q-Mix Bus 0 Input Playback Pan" and write[1] == 0
        )
        bus_enable_write = max(
            index for index, write in enumerate(io.writes)
            if write[0] == "Q-Mix Bus Playback Switch" and write[1] == 0
        )
        self.assertLess(sync_write, bus_enable_write)

    def test_stereo_pan_translates_both_sides_without_changing_width(self) -> None:
        io = FakeControl()
        pan = "Q-Mix Bus 3 Input Playback Pan"
        io.values.update({(pan, 4): 10, (pan, 5): 101})
        hints: dict[tuple[int, int], float] = {}
        set_pair_control(io, "pair.pan", 3, 4, 65.5, hints)
        self.assertEqual((io.read(pan, 4), io.read(pan, 5)), (20, 111))
        set_pair_control(io, "pair.pan", 3, 4, 128, hints)
        self.assertEqual((io.read(pan, 4), io.read(pan, 5)), (37, 128))
        set_pair_control(io, "pair.pan", 3, 4, 0, hints)
        self.assertEqual((io.read(pan, 4), io.read(pan, 5)), (0, 91))
        set_pair_control(io, "pair.width", 3, 4, 18.5, hints)
        self.assertEqual((io.read(pan, 4), io.read(pan, 5)), (91, 0))
        state = read_input_states(io, 3, (4, 5), [4], hints)[0]["stereo"]
        self.assertEqual((state["width"], state["pan"]), (18.5, 45.5))
        set_pair_control(io, "pair.width", 3, 4, 128, hints)
        self.assertEqual((io.read(pan, 4), io.read(pan, 5)), (0, 91))
        self.assertTrue(all(write[0] == pan for write in io.writes))

    def test_profile_failure_is_reported_without_reenabling_bus(self) -> None:
        io = FakeControl()
        profile = capture_profile(io)
        profile["buses"][0]["enabled"] = True
        profile["buses"][0]["routes"][0]["volume"] = 1234
        io.fail_on = ("Q-Mix Bus 0 Input Playback Volume", 0)
        with self.assertRaises(ProfileApplyError):
            apply_profile(io, profile)
        self.assertFalse(
            any(
                name == "Q-Mix Bus Playback Switch" and index == 0 and value == 1
                for name, index, value, _ in io.writes
            )
        )

    def test_profile_store_replaces_and_deletes_atomically(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = ProfileStore(Path(directory) / "profiles")
            created = store.create("Evening", {"mix": 1})
            self.assertEqual(store.get(created["id"])["snapshot"], {"mix": 1})
            store.replace(created["id"], {"mix": 2})
            self.assertEqual(store.get(created["id"])["snapshot"], {"mix": 2})
            store.delete(created["id"])
            self.assertEqual(store.list(), [])


if __name__ == "__main__":
    unittest.main()
