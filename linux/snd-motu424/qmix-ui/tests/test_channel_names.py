import tempfile
import unittest
from pathlib import Path

from qmixui.channel_names import (
    ChannelNameStore,
    ChannelNameStoreError,
    ChannelNameValidationError,
)


class ChannelNameStoreTests(unittest.TestCase):
    def test_empty_names_are_allowed_and_persisted_by_channel(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "channel-names.json"
            store = ChannelNameStore(path)
            self.assertEqual(store.read()["inputs"], [""] * 96)
            self.assertEqual(store.read()["outputs"], [""] * 48)

            names = store.replace(
                ["  Vocal mic  "] + [""] * 95,
                ["Monitors"] + [""] * 47,
            )
            reloaded = ChannelNameStore(path).read()

        self.assertEqual(names["inputs"][0], "Vocal mic")
        self.assertEqual(names["inputs"][1], "")
        self.assertEqual(names["outputs"][0], "Monitors")
        self.assertEqual(reloaded, names)

    def test_invalid_names_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = ChannelNameStore(Path(directory) / "channel-names.json")
            with self.assertRaises(ChannelNameValidationError):
                store.replace([""] * 95, [""] * 48)
            with self.assertRaises(ChannelNameValidationError):
                store.replace(["\n"] + [""] * 95, [""] * 48)
            with self.assertRaises(ChannelNameValidationError):
                store.replace(["x" * 49] + [""] * 95, [""] * 48)

    def test_corrupt_storage_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "channel-names.json"
            path.write_text("invalid json", encoding="utf-8")
            with self.assertRaises(ChannelNameStoreError):
                ChannelNameStore(path)


if __name__ == "__main__":
    unittest.main()
