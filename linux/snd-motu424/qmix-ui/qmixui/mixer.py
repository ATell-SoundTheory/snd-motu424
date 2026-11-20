import re
from collections.abc import Collection, Iterable, MutableMapping
from pathlib import Path
from typing import Any, Protocol

from .alsa import AlsaError
from .stereo import PAN_MAX, pair_balance, pair_pan, pair_pans, pair_volumes, pair_width


INPUTS = 96
BUSES = 48
PAIRS = 48


class ControlIO(Protocol):
    def close(self) -> None: ...

    def read(self, name: str, index: int, boolean: bool = False) -> int: ...

    def read_members(self, name: str, count: int) -> list[int]: ...

    def write(
        self, name: str, index: int, value: int, boolean: bool = False
    ) -> None: ...

    def hardware_controls(self) -> list[dict[str, Any]]: ...

    def write_hardware_control(
        self, name: str, index: int, values: list[int], kind: str
    ) -> None: ...


class MixerError(ValueError):
    pass


_MISSING = object()


class ProfileApplyError(RuntimeError):
    def __init__(self, message: str, applied: int) -> None:
        super().__init__(message)
        self.applied = applied


class StereoPairSyncError(RuntimeError):
    def __init__(self, message: str, applied: int) -> None:
        super().__init__(message)
        self.applied = applied


CONTROL_FIELDS: dict[str, tuple[str, bool, int, int]] = {
    "input.trim": ("Q-Mix Input Trim Volume", False, 64, 255),
    "input.enabled": ("Q-Mix Input Switch", True, 0, 1),
    "route.volume": ("Input Playback Volume", False, 0, 32768),
    "route.pan": ("Input Playback Pan", False, 0, 128),
    "route.enabled": ("Input Playback Switch", True, 0, 1),
    "route.solo": ("Input Solo Switch", True, 0, 1),
    "bus.volume": ("Q-Mix Bus Playback Volume", False, 0, 32768),
    "bus.enabled": ("Q-Mix Bus Playback Switch", True, 0, 1),
    "stereo.balance": ("Stereo Balance", False, 0, 128),
    "stereo.width": ("Stereo Width", False, 0, 128),
    "talkback.enabled": ("Q-Mix Talkback Switch", True, 0, 1),
    "talkback.input": ("Q-Mix Talkback Input", False, 0, INPUTS),
    "talkback.dim": ("Q-Mix Talkback Dim", False, 0, 128),
    "talkback.bus": ("Q-Mix Talkback Bus Switch", True, 0, 1),
    "listenback.enabled": ("Q-Mix Listenback Switch", True, 0, 1),
    "listenback.input": ("Q-Mix Listenback Input", False, 0, INPUTS),
    "listenback.dim": ("Q-Mix Listenback Dim", False, 0, 128),
    "listenback.bus": ("Q-Mix Listenback Bus Switch", True, 0, 1),
    "meter.input_bank": ("Q-Mix Meter Input Bank", True, 0, 1),
    "meter.bus": ("Q-Mix Meter Bus", False, 0, BUSES),
}


def _control_name(field: str, bus: int | None) -> str:
    name = CONTROL_FIELDS[field][0]
    if field.startswith("route."):
        return f"Q-Mix Bus {bus} {name}"
    if field.startswith("stereo."):
        return f"Q-Mix Bus {bus} {name}"
    return name


def _control_index(field: str, bus: int | None, input_index: int | None) -> int:
    if field in ("input.trim", "input.enabled"):
        if input_index is None or not 0 <= input_index < INPUTS:
            raise MixerError("input index must be between 0 and 95")
        return input_index
    if field.startswith("route."):
        if bus is None or not 0 <= bus < BUSES:
            raise MixerError("bus index must be between 0 and 47")
        if input_index is None or not 0 <= input_index < INPUTS:
            raise MixerError("input index must be between 0 and 95")
        return input_index
    if field.startswith("stereo."):
        if bus is None or not 0 <= bus < BUSES:
            raise MixerError("bus index must be between 0 and 47")
        if input_index is None or input_index % 2 or not 0 <= input_index < INPUTS:
            raise MixerError("stereo controls require an even input index")
        return input_index // 2
    if field in ("bus.volume", "bus.enabled", "talkback.bus", "listenback.bus"):
        if bus is None or not 0 <= bus < BUSES:
            raise MixerError("bus index must be between 0 and 47")
        return bus
    if field.startswith(("talkback.", "listenback.")):
        return 0
    if field.startswith("meter."):
        return 0
    raise MixerError("unknown control")


def _element(
    field: str,
    bus: int | None,
    input_index: int | None,
    value: Any = _MISSING,
) -> tuple[str, int, bool, Any]:
    if field not in CONTROL_FIELDS:
        raise MixerError("unknown control")
    name, boolean, minimum, maximum = CONTROL_FIELDS[field]
    del name
    index = _control_index(field, bus, input_index)
    if value is not _MISSING:
        if isinstance(value, bool):
            value = int(value)
        if not isinstance(value, int) or not minimum <= value <= maximum:
            raise MixerError(f"{field} must be an integer from {minimum} to {maximum}")
    return _control_name(field, bus), index, boolean, value


def _read(io: ControlIO, field: str, bus: int | None, input_index: int | None) -> int:
    name, index, boolean, _ = _element(field, bus, input_index)
    return io.read(name, index, boolean)


def set_control(
    io: ControlIO,
    field: str,
    bus: int | None,
    input_index: int | None,
    value: Any,
) -> int:
    name, index, boolean, checked = _element(field, bus, input_index, value)
    if checked is _MISSING:
        raise MixerError("control value is required")
    io.write(name, index, checked, boolean)
    return checked


def read_control(
    io: ControlIO, field: str, bus: int | None, input_index: int | None
) -> int:
    return _read(io, field, bus, input_index)


PAIR_SHARED_FIELDS = ("input.trim", "input.enabled", "route.enabled", "route.solo")
PAIR_STEREO_FIELDS = ("pair.gain", "pair.balance", "pair.width", "pair.pan")

BalanceHints = MutableMapping[tuple[int, int], float]


def _left_input(left_input: Any) -> int:
    if (
        isinstance(left_input, bool)
        or not isinstance(left_input, int)
        or not 0 <= left_input < INPUTS - 1
        or left_input % 2
    ):
        raise MixerError("left input must be an even channel index from 0 to 94")
    return left_input


def _write_if_changed(
    io: ControlIO, field: str, bus: int | None, input_index: int, value: int
) -> int:
    if _read(io, field, bus, input_index) == value:
        return 0
    set_control(io, field, bus, input_index, value)
    return 1


def sync_stereo_pair(
    io: ControlIO, left_input: int, buses: Iterable[int] | None = None
) -> int:
    """Synchronize a linked pair's shared controls, preserving levels and pans.

    The right channel takes the left channel's trim and switches.
    """
    left = _left_input(left_input)
    right = left + 1
    applied = 0
    try:
        for field in ("input.trim", "input.enabled"):
            applied += _write_if_changed(
                io, field, None, right, _read(io, field, None, left)
            )
        for bus in range(BUSES) if buses is None else buses:
            for field in ("route.enabled", "route.solo"):
                applied += _write_if_changed(
                    io, field, bus, right, _read(io, field, bus, left)
                )
    except AlsaError as error:
        raise StereoPairSyncError(
            f"stereo pair sync failed after {applied} changes: {error}",
            applied,
        ) from error
    return applied


def _pair_value(field: str, value: Any) -> Any:
    if field in ("pair.balance", "pair.width", "pair.pan"):
        if (
            isinstance(value, bool)
            or not isinstance(value, (int, float))
            or not 0 <= value <= PAN_MAX
        ):
            raise MixerError(f"{field} must be a number from 0 to 128")
        if field != "pair.balance" and value * 2 != round(value * 2):
            raise MixerError(f"{field} must use half-step increments")
        return value
    maximum = CONTROL_FIELDS["route.volume"][3] if field == "pair.gain" else PAN_MAX
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= maximum:
        raise MixerError(f"{field} must be an integer from 0 to {maximum}")
    return value


def set_pair_control(
    io: ControlIO,
    field: str,
    bus: int | None,
    left_input: int,
    value: Any,
    hints: BalanceHints,
) -> None:
    """Write one control of a linked pair.

    Trim and switches are written to both channels. Gain, balance, width and pan
    are converted to the two route volumes and pans; the other stereo
    values are kept.
    """
    left = _left_input(left_input)
    inputs = (left, left + 1)
    applied = 0
    try:
        if field in PAIR_SHARED_FIELDS:
            if field.startswith("input.") and bus is not None:
                raise MixerError("input controls do not accept a bus")
            for input_index in inputs:
                set_control(io, field, bus, input_index, value)
                applied += 1
            return
        if field not in PAIR_STEREO_FIELDS:
            raise MixerError("control cannot be linked across a stereo pair")
        value = _pair_value(field, value)
        if field in ("pair.width", "pair.pan"):
            pans = [_read(io, "route.pan", bus, index) for index in inputs]
            width, pan = pair_width(*pans), pair_pan(*pans)
            if field == "pair.pan":
                # Whole-step translation preserves separation, including odd pairs.
                delta = max(-min(pans), min(PAN_MAX - max(pans), round(value - pan)))
                next_pans = tuple(current + delta for current in pans)
            else:
                # Keep the midpoint fixed; limit separation at either pan boundary.
                radius = min(pan, PAN_MAX - pan)
                delta = round(value - width)
                width = max(64 - radius, min(64 + radius, width + delta))
                next_pans = pair_pans(width, pan)
            for input_index, next_pan in zip(inputs, next_pans):
                applied += _write_if_changed(
                    io, "route.pan", bus, input_index, next_pan
                )
            return
        volumes = [_read(io, "route.volume", bus, index) for index in inputs]
        gain = value if field == "pair.gain" else max(volumes)
        balance = (
            value
            if field == "pair.balance"
            else pair_balance(*volumes, hints.get((bus, left)))
        )
        hints[(bus, left)] = balance
        for input_index, volume in zip(inputs, pair_volumes(gain, balance)):
            applied += _write_if_changed(io, "route.volume", bus, input_index, volume)
    except AlsaError as error:
        raise StereoPairSyncError(
            f"stereo pair control failed after {applied} changes: {error}",
            applied,
        ) from error


def read_input_states(
    io: ControlIO,
    bus: int | None,
    inputs: Iterable[int],
    linked_pairs: Collection[int] = (),
    hints: BalanceHints | None = None,
) -> list[dict[str, Any]]:
    """Read inputs; with a bus, include their routes to it.

    The left input of each pair in ``linked_pairs`` also gets the pair's
    stereo controls, provided its right input is read too.
    """
    states: dict[int, dict[str, Any]] = {}
    for index in inputs:
        state: dict[str, Any] = {
            "index": index,
            "trim": _read(io, "input.trim", None, index),
            "enabled": bool(_read(io, "input.enabled", None, index)),
        }
        if bus is not None:
            state["route"] = {
                "volume": _read(io, "route.volume", bus, index),
                "pan": _read(io, "route.pan", bus, index),
                "enabled": bool(_read(io, "route.enabled", bus, index)),
                "solo": bool(_read(io, "route.solo", bus, index)),
            }
            state["stereo"] = None
        states[index] = state
    if bus is not None:
        for left in linked_pairs:
            if left not in states or left + 1 not in states:
                continue
            routes = [states[index]["route"] for index in (left, left + 1)]
            volumes = [route["volume"] for route in routes]
            states[left]["stereo"] = {
                "gain": max(volumes),
                "balance": pair_balance(
                    *volumes, hints.get((bus, left)) if hints else None
                ),
                "width": pair_width(*(route["pan"] for route in routes)),
                "pan": pair_pan(*(route["pan"] for route in routes)),
            }
    return list(states.values())


def _resolve_proc_card(card: str, root: Path) -> Path | None:
    cards = root / "cards"
    if not cards.is_file():
        return None
    requested = card.removeprefix("hw:").split(",", 1)[0]
    if requested.isdigit():
        return root / f"card{requested}" / "motu424"
    for line in cards.read_text(errors="replace").splitlines():
        match = re.match(r"\s*(\d+)\s+\[([^\]]+)\]:", line)
        if match and (match.group(2).strip() == requested or requested in match.group(2)):
            return root / f"card{match.group(1)}" / "motu424"
    return None


def read_topology(card: str, proc_root: Path = Path("/proc/asound")) -> dict[str, Any]:
    proc_file = _resolve_proc_card(card, proc_root)
    ports = [
        {"index": port, "name": f"Port {port}", "configured": None}
        for port in range(4)
    ]
    inputs: list[bool | None] = [None] * INPUTS
    outputs: list[bool | None] = [None] * (BUSES * 2)
    if proc_file is None or not proc_file.is_file():
        topology = {
            "known": False,
            "ports": ports,
            "inputs": inputs,
            "outputs": [None] * BUSES,
        }
        label_inputs(topology)
        return topology

    section: str | None = None
    seen_input = False
    seen_output = False
    lines = proc_file.read_text(errors="replace").splitlines()
    for line in lines:
        port_match = re.match(r"\s*Port (\d+):\s*([^,\n]+)(?:,\s*(.*))?", line)
        if port_match and int(port_match.group(1)) < 4:
            port = int(port_match.group(1))
            state = (port_match.group(3) or "").lower()
            name = port_match.group(2).strip()
            if name.startswith(("not connected", "detected", "inquiry sent", "reading description")):
                name = f"Port {port}"
            ports[port] = {
                "index": port,
                "name": name,
                "configured": "configured" in state and "not configured" not in state,
            }
            continue
        if line.startswith("Capture:"):
            section = "capture"
            seen_input = True
            continue
        if line.startswith("Playback:"):
            section = "playback"
            seen_output = True
            continue
        if section not in ("capture", "playback"):
            continue
        channel_match = re.search(
            r"\d+-\d+:\s+port (\d+),.*?\s(\d+)-(\d+)\s*$", line
        )
        if not channel_match:
            continue
        port, first, last = map(int, channel_match.groups())
        if port >= 4:
            continue
        target = inputs if section == "capture" else outputs
        offset = port * 24
        for channel in range(first, last + 1):
            if 1 <= channel <= 24:
                target[offset + channel - 1] = True

    if seen_input:
        inputs = [value is True for value in inputs]
    if seen_output:
        outputs = [
            outputs[index * 2] is True and outputs[index * 2 + 1] is True
            for index in range(BUSES)
        ]
    topology = {
        "known": seen_input or seen_output,
        "card_name": lines[0].split(" at ", 1)[0] if lines and lines[0].startswith("MOTU ") else "PCI(e)-424",
        "ports": ports,
        "inputs": inputs,
        "outputs": outputs,
    }
    label_inputs(topology)
    return topology


def label_inputs(topology: dict[str, Any]) -> None:
    topology["input_short_labels"] = [
        input_source_label(topology, index) for index in range(INPUTS)
    ]
    topology["input_labels"] = [
        input_label(topology, index) for index in range(INPUTS)
    ]


def input_label(topology: dict[str, Any], physical_index: int) -> str:
    source = input_source_label(topology, physical_index)
    return f"Port {physical_index // 24 + 1:02d} · {source}"


def input_source_label(topology: dict[str, Any], physical_index: int) -> str:
    port = physical_index // 24
    channel = physical_index % 24 + 1
    port_info = topology["ports"][port]
    name = port_info["name"]
    normalized_name = name.casefold().replace(" ", "")
    source = f"{name} Input {channel:02d}"
    if normalized_name in {"24i/o", "24i"}:
        source = f"Analog {channel}"
    elif name == "2408" and port_info.get("analog_bank") in range(3):
        bank = (channel - 1) // 8
        channel_in_bank = (channel - 1) % 8 + 1
        analog_bank = port_info["analog_bank"]
        if bank == analog_bank:
            source = f"Analog {channel_in_bank}"
        else:
            optical_bank = bank if bank < analog_bank else bank - 1
            optical_name = chr(ord("A") + optical_bank)
            formats = port_info.get("bank_formats", [])
            bank_format = formats[bank] if bank < len(formats) else 0
            if bank_format == 0:
                source = f"Optical {optical_name}{channel_in_bank}"
            elif bank_format == 1:
                source = f"TDIF {optical_name}{channel_in_bank}"
            elif bank_format == 2 and bank == 2 and channel_in_bank <= 2:
                source = f"S/PDIF {'L' if channel_in_bank == 1 else 'R'}"
    elif name.startswith("Port "):
        source = f"Input {channel:02d}"
    return source


def _bus_label(topology: dict[str, Any], bus: int) -> str:
    port = bus // 12
    pair = bus % 12 + 1
    name = topology["ports"][port]["name"]
    return f"Port {port + 1:02d} · {name} · Out {pair:02d}"


def read_bus_state(
    io: ControlIO,
    bus: int,
    topology: dict[str, Any],
    linked_pairs: Collection[int] = (),
    hints: BalanceHints | None = None,
) -> dict[str, Any]:
    if not 0 <= bus < BUSES:
        raise MixerError("bus index must be between 0 and 47")
    inputs = [
        {
            "index": state["index"],
            "label": input_label(topology, state["index"]),
            "available": topology["inputs"][state["index"]],
            **state,
        }
        for state in read_input_states(io, bus, range(INPUTS), linked_pairs, hints)
    ]
    talkback = _read_feature(io, "talkback")
    listenback = _read_feature(io, "listenback")
    return {
        "bus": {
            "index": bus,
            "label": _bus_label(topology, bus),
            "available": topology["outputs"][bus],
            "volume": _read(io, "bus.volume", bus, None),
            "enabled": bool(_read(io, "bus.enabled", bus, None)),
        },
        "inputs": inputs,
        "talkback": talkback,
        "listenback": listenback,
        "meter": {
            "input_bank": _read(io, "meter.input_bank", None, None),
            "bus": _read(io, "meter.bus", None, None),
        },
        "topology": topology,
    }


def _read_feature(io: ControlIO, feature: str) -> dict[str, Any]:
    return {
        "enabled": bool(_read(io, f"{feature}.enabled", None, None)),
        "input": _read(io, f"{feature}.input", None, None),
        "dim": _read(io, f"{feature}.dim", None, None),
        "buses": [
            bool(_read(io, f"{feature}.bus", bus, None)) for bus in range(BUSES)
        ],
    }


def read_meters(io: ControlIO) -> dict[str, Any]:
    input_bank = _read(io, "meter.input_bank", None, None)
    bus = _read(io, "meter.bus", None, None)
    return {
        "input_bank": input_bank,
        "bus": bus,
        "values": io.read_members("Q-Mix DSP Peak", 52),
    }


def capture_profile(io: ControlIO) -> dict[str, Any]:
    snapshot: dict[str, Any] = {
        "inputs": [],
        "buses": [],
        "talkback": _read_feature(io, "talkback"),
        "listenback": _read_feature(io, "listenback"),
    }
    for input_index in range(INPUTS):
        snapshot["inputs"].append(
            {
                "trim": _read(io, "input.trim", None, input_index),
                "enabled": bool(_read(io, "input.enabled", None, input_index)),
            }
        )
    for bus in range(BUSES):
        routes = []
        for input_index in range(INPUTS):
            routes.append(
                {
                    "volume": _read(io, "route.volume", bus, input_index),
                    "pan": _read(io, "route.pan", bus, input_index),
                    "enabled": bool(_read(io, "route.enabled", bus, input_index)),
                    "solo": bool(_read(io, "route.solo", bus, input_index)),
                }
            )
        snapshot["buses"].append(
            {
                "volume": _read(io, "bus.volume", bus, None),
                "enabled": bool(_read(io, "bus.enabled", bus, None)),
                "routes": routes,
                "stereo": [
                    {
                        "balance": _read(io, "stereo.balance", bus, i),
                        "width": _read(io, "stereo.width", bus, i),
                    }
                    for i in range(0, INPUTS, 2)
                ],
            }
        )
    return snapshot


def validate_profile(profile: Any) -> None:
    if not isinstance(profile, dict):
        raise MixerError("profile data must be an object")
    inputs = profile.get("inputs")
    buses = profile.get("buses")
    if not isinstance(inputs, list) or not isinstance(buses, list):
        raise MixerError("profile inputs and buses must be arrays")
    if len(inputs) != INPUTS or len(buses) != BUSES:
        raise MixerError("profile has an unsupported input or bus count")
    for entry in inputs:
        if not isinstance(entry, dict):
            raise MixerError("profile input settings must be objects")
        _validate_value("input.trim", entry.get("trim"))
        _validate_value("input.enabled", entry.get("enabled"))
    for bus in buses:
        if not isinstance(bus, dict):
            raise MixerError("profile bus settings must be objects")
        _validate_value("bus.volume", bus.get("volume"))
        _validate_value("bus.enabled", bus.get("enabled"))
        routes = bus.get("routes")
        stereo = bus.get("stereo")
        if (
            not isinstance(routes, list)
            or not isinstance(stereo, list)
            or len(routes) != INPUTS
            or len(stereo) != PAIRS
        ):
            raise MixerError("profile has an unsupported route or stereo count")
        for route in routes:
            if not isinstance(route, dict):
                raise MixerError("profile route settings must be objects")
            for field in ("volume", "pan", "enabled", "solo"):
                _validate_value(f"route.{field}", route.get(field))
        for pair in stereo:
            if not isinstance(pair, dict):
                raise MixerError("profile stereo settings must be objects")
            for field in ("balance", "width"):
                _validate_value(f"stereo.{field}", pair.get(field))
    for feature in ("talkback", "listenback"):
        values = profile.get(feature)
        if (
            not isinstance(values, dict)
            or not isinstance(values.get("buses"), list)
            or len(values["buses"]) != BUSES
        ):
            raise MixerError(f"profile is missing {feature} settings")
        for field in ("enabled", "input", "dim"):
            _validate_value(f"{feature}.{field}", values.get(field))
        for enabled in values["buses"]:
            _validate_value(f"{feature}.bus", enabled)


def _validate_value(field: str, value: Any) -> None:
    _element(field, 0 if field.startswith(("route.", "stereo.", "bus.", "talkback.bus", "listenback.bus")) else None,
             0 if field.startswith(("route.", "stereo.", "input.")) else None, value)


def profile_difference_count(left: dict[str, Any], right: dict[str, Any]) -> int:
    validate_profile(left)
    validate_profile(right)
    return _difference_count(left, right)


def _difference_count(left: Any, right: Any) -> int:
    if isinstance(left, dict) and isinstance(right, dict):
        return sum(_difference_count(left[key], right[key]) for key in left)
    if isinstance(left, list) and isinstance(right, list):
        return sum(_difference_count(a, b) for a, b in zip(left, right))
    return int(left != right)


def apply_profile(
    io: ControlIO, profile: Any, linked_pairs: tuple[int, ...] | list[int] = ()
) -> int:
    validate_profile(profile)
    current = capture_profile(io)
    applied = 0

    def write_if_changed(
        field: str,
        bus: int | None,
        input_index: int | None,
        value: Any,
        old_value: Any,
    ) -> None:
        nonlocal applied
        if value != old_value:
            set_control(io, field, bus, input_index, value)
            applied += 1

    try:
        for bus in range(BUSES):
            if current["buses"][bus]["enabled"]:
                set_control(io, "bus.enabled", bus, None, 0)
                applied += 1
        for feature in ("talkback", "listenback"):
            if current[feature]["enabled"]:
                set_control(io, f"{feature}.enabled", None, None, 0)
                applied += 1
        for input_index, entry in enumerate(profile["inputs"]):
            old = current["inputs"][input_index]
            write_if_changed("input.trim", None, input_index, entry["trim"], old["trim"])
            write_if_changed(
                "input.enabled", None, input_index, entry["enabled"], old["enabled"]
            )
        for bus_index, bus in enumerate(profile["buses"]):
            old_bus = current["buses"][bus_index]
            write_if_changed(
                "bus.volume", bus_index, None, bus["volume"], old_bus["volume"]
            )
            changed_stereo_pairs: set[int] = set()
            for pair_index, pair in enumerate(bus["stereo"]):
                old_pair = old_bus["stereo"][pair_index]
                input_index = pair_index * 2
                for field in ("balance", "width"):
                    if pair[field] != old_pair[field]:
                        changed_stereo_pairs.add(pair_index)
                    write_if_changed(
                        f"stereo.{field}",
                        bus_index,
                        input_index,
                        pair[field],
                        old_pair[field],
                    )
            for input_index, route in enumerate(bus["routes"]):
                old_route = old_bus["routes"][input_index]
                for field in ("volume", "pan", "enabled", "solo"):
                    if (
                        field in ("volume", "pan")
                        and input_index // 2 in changed_stereo_pairs
                    ):
                        set_control(
                            io, f"route.{field}", bus_index, input_index, route[field]
                        )
                        applied += 1
                    else:
                        write_if_changed(
                            f"route.{field}",
                            bus_index,
                            input_index,
                            route[field],
                            old_route[field],
                        )
        for feature in ("talkback", "listenback"):
            values = profile[feature]
            old_values = current[feature]
            write_if_changed(
                f"{feature}.input", None, None, values["input"], old_values["input"]
            )
            write_if_changed(
                f"{feature}.dim", None, None, values["dim"], old_values["dim"]
            )
            for bus_index, enabled in enumerate(values["buses"]):
                write_if_changed(
                    f"{feature}.bus",
                    bus_index,
                    None,
                    enabled,
                    old_values["buses"][bus_index],
                )
        for left_input in linked_pairs:
            try:
                applied += sync_stereo_pair(io, left_input)
            except StereoPairSyncError as error:
                raise ProfileApplyError(
                    f"{error}; mixer state may be partially updated, verify before continuing",
                    applied + error.applied,
                ) from error
        for feature in ("talkback", "listenback"):
            write_if_changed(
                f"{feature}.enabled",
                None,
                None,
                profile[feature]["enabled"],
                False,
            )
        for bus_index, bus in enumerate(profile["buses"]):
            write_if_changed(
                "bus.enabled",
                bus_index,
                None,
                bus["enabled"],
                False,
            )
        return applied
    except (AlsaError, MixerError) as error:
        raise ProfileApplyError(
            f"{error}; mixer state may be partially updated, verify before continuing",
            applied,
        ) from error
