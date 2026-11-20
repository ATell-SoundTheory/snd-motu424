import ctypes
import ctypes.util
from typing import Any


class AlsaError(RuntimeError):
    pass


class AlsaControl:
    def __init__(self, card: str) -> None:
        library = ctypes.util.find_library("asound")
        if not library:
            raise AlsaError("libasound was not found; install the ALSA runtime library")
        self._lib = ctypes.CDLL(library)
        self._configure()
        self._handle = ctypes.c_void_p()
        name = card if ":" in card else f"hw:{card}"
        result = self._lib.snd_ctl_open(
            ctypes.byref(self._handle), name.encode(), 0
        )
        self._check(result, f"open ALSA control {name}")

    def _configure(self) -> None:
        lib = self._lib
        lib.snd_ctl_open.argtypes = [
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.c_char_p,
            ctypes.c_int,
        ]
        lib.snd_ctl_open.restype = ctypes.c_int
        lib.snd_ctl_close.argtypes = [ctypes.c_void_p]
        lib.snd_ctl_close.restype = ctypes.c_int
        lib.snd_ctl_elem_value_malloc.argtypes = [
            ctypes.POINTER(ctypes.c_void_p)
        ]
        lib.snd_ctl_elem_value_malloc.restype = ctypes.c_int
        lib.snd_ctl_elem_value_free.argtypes = [ctypes.c_void_p]
        lib.snd_ctl_elem_value_free.restype = None
        lib.snd_ctl_elem_value_set_interface.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
        ]
        lib.snd_ctl_elem_value_set_interface.restype = None
        lib.snd_ctl_elem_value_set_device.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint,
        ]
        lib.snd_ctl_elem_value_set_device.restype = None
        lib.snd_ctl_elem_value_set_subdevice.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint,
        ]
        lib.snd_ctl_elem_value_set_subdevice.restype = None
        lib.snd_ctl_elem_value_set_name.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
        ]
        lib.snd_ctl_elem_value_set_name.restype = None
        lib.snd_ctl_elem_value_set_index.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint,
        ]
        lib.snd_ctl_elem_value_set_index.restype = None
        for suffix, value_type in (
            ("boolean", ctypes.c_long),
            ("integer", ctypes.c_long),
            ("enumerated", ctypes.c_uint),
        ):
            setter = getattr(lib, f"snd_ctl_elem_value_set_{suffix}")
            setter.argtypes = [ctypes.c_void_p, ctypes.c_uint, value_type]
            setter.restype = None
            getter = getattr(lib, f"snd_ctl_elem_value_get_{suffix}")
            getter.argtypes = [ctypes.c_void_p, ctypes.c_uint]
            getter.restype = value_type
        lib.snd_ctl_elem_read.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        lib.snd_ctl_elem_read.restype = ctypes.c_int
        lib.snd_ctl_elem_write.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        lib.snd_ctl_elem_write.restype = ctypes.c_int
        lib.snd_strerror.argtypes = [ctypes.c_int]
        lib.snd_strerror.restype = ctypes.c_char_p
        pointer = ctypes.c_void_p
        uint = ctypes.c_uint
        for name, result, arguments in (
            ("snd_ctl_elem_list_malloc", ctypes.c_int, [ctypes.POINTER(pointer)]),
            ("snd_ctl_elem_list_free", None, [pointer]),
            ("snd_ctl_elem_list", ctypes.c_int, [pointer, pointer]),
            ("snd_ctl_elem_list_get_count", uint, [pointer]),
            ("snd_ctl_elem_list_alloc_space", ctypes.c_int, [pointer, uint]),
            ("snd_ctl_elem_list_free_space", None, [pointer]),
            ("snd_ctl_elem_list_get_name", ctypes.c_char_p, [pointer, uint]),
            ("snd_ctl_elem_list_get_index", uint, [pointer, uint]),
            ("snd_ctl_elem_info_malloc", ctypes.c_int, [ctypes.POINTER(pointer)]),
            ("snd_ctl_elem_info_free", None, [pointer]),
            ("snd_ctl_elem_info_set_interface", None, [pointer, ctypes.c_int]),
            ("snd_ctl_elem_info_set_name", None, [pointer, ctypes.c_char_p]),
            ("snd_ctl_elem_info_set_index", None, [pointer, uint]),
            ("snd_ctl_elem_info", ctypes.c_int, [pointer, pointer]),
            ("snd_ctl_elem_info_get_type", ctypes.c_int, [pointer]),
            ("snd_ctl_elem_info_get_count", uint, [pointer]),
            ("snd_ctl_elem_info_is_readable", ctypes.c_int, [pointer]),
            ("snd_ctl_elem_info_is_writable", ctypes.c_int, [pointer]),
            ("snd_ctl_elem_info_is_inactive", ctypes.c_int, [pointer]),
            ("snd_ctl_elem_info_get_min", ctypes.c_long, [pointer]),
            ("snd_ctl_elem_info_get_max", ctypes.c_long, [pointer]),
            ("snd_ctl_elem_info_get_step", ctypes.c_long, [pointer]),
            ("snd_ctl_elem_info_get_items", uint, [pointer]),
            ("snd_ctl_elem_info_set_item", None, [pointer, uint]),
            ("snd_ctl_elem_info_get_item_name", ctypes.c_char_p, [pointer]),
        ):
            function = getattr(lib, name)
            function.restype = result
            function.argtypes = arguments

    def _check(self, result: int, operation: str) -> None:
        if result >= 0:
            return
        detail = self._lib.snd_strerror(result).decode("utf-8", "replace")
        raise AlsaError(f"{operation}: {detail} ({result})")

    def _new_value(self, name: str, index: int) -> ctypes.c_void_p:
        value = ctypes.c_void_p()
        self._check(
            self._lib.snd_ctl_elem_value_malloc(ctypes.byref(value)),
            "allocate ALSA control value",
        )
        self._lib.snd_ctl_elem_value_set_interface(value, 2)
        self._lib.snd_ctl_elem_value_set_device(value, 0)
        self._lib.snd_ctl_elem_value_set_subdevice(value, 0)
        self._lib.snd_ctl_elem_value_set_name(value, name.encode())
        self._lib.snd_ctl_elem_value_set_index(value, index)
        return value

    def read(self, name: str, index: int, boolean: bool = False) -> int:
        value = self._new_value(name, index)
        try:
            self._check(
                self._lib.snd_ctl_elem_read(self._handle, value),
                f"read {name}[{index}]",
            )
            getter = (
                self._lib.snd_ctl_elem_value_get_boolean
                if boolean
                else self._lib.snd_ctl_elem_value_get_integer
            )
            return int(getter(value, 0))
        finally:
            self._lib.snd_ctl_elem_value_free(value)

    def read_members(self, name: str, count: int) -> list[int]:
        value = self._new_value(name, 0)
        try:
            self._check(
                self._lib.snd_ctl_elem_read(self._handle, value),
                f"read {name}",
            )
            return [
                int(self._lib.snd_ctl_elem_value_get_integer(value, i))
                for i in range(count)
            ]
        finally:
            self._lib.snd_ctl_elem_value_free(value)

    def write(
        self, name: str, index: int, value: int, boolean: bool = False
    ) -> None:
        element = self._new_value(name, index)
        try:
            setter = (
                self._lib.snd_ctl_elem_value_set_boolean
                if boolean
                else self._lib.snd_ctl_elem_value_set_integer
            )
            setter(element, 0, value)
            self._check(
                self._lib.snd_ctl_elem_write(self._handle, element),
                f"write {name}[{index}]",
            )
        finally:
            self._lib.snd_ctl_elem_value_free(element)

    def hardware_controls(self) -> list[dict[str, Any]]:
        """Discover only hardware settings, never read consuming Q-Mix peaks."""
        lib = self._lib
        elements = ctypes.c_void_p()
        self._check(lib.snd_ctl_elem_list_malloc(ctypes.byref(elements)), "allocate control list")
        try:
            self._check(lib.snd_ctl_elem_list(self._handle, elements), "count ALSA controls")
            count = lib.snd_ctl_elem_list_get_count(elements)
            self._check(lib.snd_ctl_elem_list_alloc_space(elements, count), "allocate control IDs")
            self._check(lib.snd_ctl_elem_list(self._handle, elements), "list ALSA controls")
            controls = []
            for position in range(count):
                name = lib.snd_ctl_elem_list_get_name(elements, position).decode()
                if not name.startswith(("Card ", "Interface ", "SMPTE ")):
                    continue
                index = lib.snd_ctl_elem_list_get_index(elements, position)
                control = self._hardware_control(name, index)
                if control is not None:
                    controls.append(control)
            return controls
        finally:
            lib.snd_ctl_elem_list_free_space(elements)
            lib.snd_ctl_elem_list_free(elements)

    def _hardware_control(self, name: str, index: int) -> dict[str, Any] | None:
        lib = self._lib
        info = ctypes.c_void_p()
        self._check(lib.snd_ctl_elem_info_malloc(ctypes.byref(info)), "allocate control info")
        try:
            lib.snd_ctl_elem_info_set_interface(info, 2)
            lib.snd_ctl_elem_info_set_name(info, name.encode())
            lib.snd_ctl_elem_info_set_index(info, index)
            self._check(lib.snd_ctl_elem_info(self._handle, info), f"inspect {name}[{index}]")
            kind = {1: "boolean", 2: "integer", 3: "enumerated"}.get(
                lib.snd_ctl_elem_info_get_type(info)
            )
            if (
                kind is None
                or not lib.snd_ctl_elem_info_is_readable(info)
                or lib.snd_ctl_elem_info_is_inactive(info)
            ):
                return None
            options = []
            if kind == "enumerated":
                for item in range(lib.snd_ctl_elem_info_get_items(info)):
                    lib.snd_ctl_elem_info_set_item(info, item)
                    self._check(lib.snd_ctl_elem_info(self._handle, info), f"inspect {name} option")
                    options.append({
                        "value": item,
                        "label": lib.snd_ctl_elem_info_get_item_name(info).decode(),
                    })
            value = self._new_value(name, index)
            try:
                self._check(lib.snd_ctl_elem_read(self._handle, value), f"read {name}[{index}]")
                getter = getattr(lib, f"snd_ctl_elem_value_get_{kind}")
                values = [
                    int(getter(value, member))
                    for member in range(lib.snd_ctl_elem_info_get_count(info))
                ]
            finally:
                lib.snd_ctl_elem_value_free(value)
            return {
                "name": name,
                "index": index,
                "kind": kind,
                "writable": bool(lib.snd_ctl_elem_info_is_writable(info)),
                "values": values,
                "min": 0 if kind != "integer" else lib.snd_ctl_elem_info_get_min(info),
                "max": (
                    len(options) - 1 if kind == "enumerated" else
                    1 if kind == "boolean" else lib.snd_ctl_elem_info_get_max(info)
                ),
                "step": max(1, lib.snd_ctl_elem_info_get_step(info)) if kind == "integer" else 1,
                "options": options,
            }
        finally:
            lib.snd_ctl_elem_info_free(info)

    def write_hardware_control(
        self, name: str, index: int, values: list[int], kind: str
    ) -> None:
        element = self._new_value(name, index)
        try:
            setter = getattr(self._lib, f"snd_ctl_elem_value_set_{kind}")
            for member, value in enumerate(values):
                setter(element, member, value)
            self._check(
                self._lib.snd_ctl_elem_write(self._handle, element),
                f"write {name}[{index}]",
            )
        finally:
            self._lib.snd_ctl_elem_value_free(element)

    def close(self) -> None:
        if self._handle:
            self._lib.snd_ctl_close(self._handle)
            self._handle = ctypes.c_void_p()

    def __enter__(self) -> "AlsaControl":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()
