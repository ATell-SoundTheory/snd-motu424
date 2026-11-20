"""Presentation and validation of the hardware controls published by ALSA."""

from typing import Any

from .mixer import MixerError


CHOICES = {
    "Card Clock Source": {
        0: "Internal", 2: "ADAT sync", 3: "SMPTE (LTC)",
        **{4 + port: f"Video · AudioWire {port + 1}" for port in range(4)},
        **{8 + port: f"AudioWire {port + 1}" for port in range(4)},
    },
    "Card S/MUX Type": {0: "Type II (default)", 1: "Type I"},
    "Interface 2408 Analog Bank": {0: "A", 1: "B", 2: "C", 3: "None"},
    "Interface 2408 Analog Mirror Bank": {0: "A", 1: "B", 2: "C"},
    "Interface 2408 Bank A Format": {0: "ADAT", 1: "TDIF"},
    "Interface 2408 Bank B Format": {0: "ADAT", 1: "TDIF"},
    "Interface 2408 Bank C Format": {0: "ADAT", 1: "TDIF", 2: "S/PDIF"},
    "Interface 2408 Clock Source": {
        0: "Word clock in", 1: "Bank A optical", 2: "Bank B optical",
        3: "Bank C optical", 4: "S/PDIF", 5: "Follow card",
    },
    "Interface 1296 Clock Source": {
        0: "Internal", 1: "Word clock in", 2: "AES/EBU", 3: "Follow card",
    },
    "Interface Word Clock Out Rate": {
        0: "System rate", 1: "44.1 / 48 kHz", 2: "88.2 / 96 kHz",
    },
    "Interface AES Output SRC Mode": {
        code: f"Mode {code}" for code in (*range(6), *range(8, 14))
    },
    "SMPTE Frame Format": {
        0: "24 fps", 1: "25 fps", 2: "29.97 drop", 3: "29.97 non-drop", 4: "30 fps",
    },
    "SMPTE Detected Format": {
        0: "None", 1: "24 fps", 2: "25 fps", 3: "29.97 drop", 4: "30 / 29.97 non-drop",
    },
}


def describe_control(control: dict[str, Any], topology: dict[str, Any]) -> dict[str, Any]:
    result = dict(control)
    name = control["name"]
    result["label"] = name.removeprefix("Interface ").removeprefix("Card ")
    if name.startswith("Interface "):
        for model in ("2408mk3 ", "2408 ", "24I/O ", "1296 "):
            result["label"] = result["label"].removeprefix(model)
    choices = CHOICES.get(name)
    if name == "Card Clock Source":
        choices = {
            value: label for value, label in CHOICES[name].items()
            if not 4 <= value <= 7 or value - 4 in topology["smpte_jack_ports"]
        }
    if "-10dBV Switch" in name:
        result["label"] = result["label"].replace(" -10dBV Switch", " reference")
        choices = {0: "+4 dBu", 1: "-10 dBV"}
    if name in ("SMPTE Reader Source", "SMPTE Generator Destination"):
        choices = {0: "Off"}
        availability = topology["outputs"] if "Destination" in name else topology["inputs"]
        for index in range(96):
            present = availability[index // 2] if "Destination" in name else availability[index]
            if present is True:
                choices[index + 1] = f"AudioWire {index // 24 + 1} · Channel {index % 24 + 1}"
        result["note"] = "Physical channel codes; the driver validates channel and jack support."
        # Dedicated 2408mk3 jacks occupy codes 97–100.
        choices.update({
            97 + port: f"SMPTE jack · AudioWire {port + 1}"
            for port in topology["smpte_jack_ports"]
        })
    if choices is not None:
        result["options"] = [
            {"value": value, "label": label}
            for value, label in choices.items()
            if control["min"] <= value <= control["max"]
        ]
    if name.startswith("SMPTE Freewheel"):
        result["label"] += " (ms; -1 = forever)"
    if name == "SMPTE Generator Audio Overwritten":
        result["clear_only"] = True
    if name in ("Interface Peak Hold Time", "Interface Clip Hold Time", "Interface AES Output Clock"):
        result["note"] = "Driver code; physical units or mode names are not yet established."
    return result


def group_devices(
    controls: list[dict[str, Any]], topology: dict[str, Any]
) -> dict[str, Any]:
    topology = {
        **topology,
        "smpte_jack_ports": sorted({
            control["index"] for control in controls
            if control["name"].startswith("Interface 2408mk3 ")
        }),
    }
    card = {
        "name": topology.get("card_name", "PCI(e)-424"), "port": None,
        "configured": True, "controls": [],
    }
    devices = [
        {"name": port["name"], "port": port["index"],
         "configured": port["configured"], "controls": []}
        for port in topology["ports"]
        if port["name"] and port["name"] != "none" and not port["name"].startswith("Port ")
    ]
    ports = {device["port"]: device for device in devices}
    for control in controls:
        target = ports.get(control["index"]) if control["name"].startswith("Interface ") else card
        if target is not None:
            target["controls"].append(describe_control(control, topology))
    return {"devices": [card, *devices]}


def validate_setting(control: dict[str, Any], values: Any) -> list[int]:
    if not control["writable"]:
        raise MixerError("this hardware setting is read-only")
    if not isinstance(values, list) or len(values) != len(control["values"]):
        raise MixerError("hardware setting has an invalid value count")
    if control.get("clear_only") and values != [0]:
        raise MixerError("this hardware notification can only be cleared")
    options = {option["value"] for option in control["options"]}
    for value in values:
        if (
            isinstance(value, bool) or not isinstance(value, int)
            or not control["min"] <= value <= control["max"]
            or (value - control["min"]) % control["step"]
            or (options and value not in options)
        ):
            raise MixerError(f"invalid value for {control['name']}")
    return values
