import json
import tempfile
import unittest
from pathlib import Path

from qmixui.stereo_pairs import StereoPairStore, StereoPairStoreError


class StereoPairStoreTests(unittest.TestCase):
    def test_pair_state_persists_by_even_input_index(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "stereo-pairs.json"
            store = StereoPairStore(path)

            updated = store.set_pair(2, True)

            self.assertEqual(len(updated), 48)
            self.assertTrue(updated[1])
            self.assertFalse(updated[0])
            self.assertTrue(StereoPairStore(path).is_enabled(2))

    def test_pair_state_rejects_invalid_indices_and_values(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = StereoPairStore(Path(directory) / "stereo-pairs.json")
            for left_input in (-2, 1, 96, True):
                with self.subTest(left_input=left_input), self.assertRaises(ValueError):
                    store.set_pair(left_input, True)
            with self.assertRaises(ValueError):
                store.set_pair(0, 1)

    def test_corrupt_storage_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "stereo-pairs.json"
            path.write_text(json.dumps({"pairs": [True]}), encoding="utf-8")

            with self.assertRaises(StereoPairStoreError):
                StereoPairStore(path)


if __name__ == "__main__":
    unittest.main()
