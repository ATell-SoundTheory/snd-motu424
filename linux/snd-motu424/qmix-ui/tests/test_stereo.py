import random
import unittest

from qmixui.stereo import pair_balance, pair_pan, pair_pans, pair_volumes, pair_width


class StereoConversionTests(unittest.TestCase):
    def test_volumes_survive_conversion_to_gain_and_balance(self) -> None:
        generator = random.Random(424)
        volumes = [(0, 0), (0, 32768), (32768, 0), (1, 2), (32767, 32768)]
        volumes += [
            (generator.randint(0, 32768), generator.randint(0, 32768))
            for _ in range(20000)
        ]
        for left, right in volumes:
            balance = pair_balance(left, right)
            self.assertEqual(
                pair_volumes(max(left, right), balance), (left, right), (left, right)
            )
            self.assertEqual(balance > 64, right > left)
            self.assertEqual(balance < 64, left > right)

    def test_set_balance_is_reported_back_at_any_gain(self) -> None:
        for gain in (0, 1, 7, 100, 32768):
            for balance in range(129):
                volumes = pair_volumes(gain, balance)
                self.assertEqual(max(volumes), gain)
                self.assertEqual(pair_balance(*volumes, hint=balance), balance)

    def test_stale_balance_hint_is_ignored(self) -> None:
        self.assertEqual(pair_balance(20000, 20000, hint=96), 64)
        self.assertAlmostEqual(pair_balance(15000, 20000, hint=10), 96)

    def test_balance_ends_silence_one_side(self) -> None:
        self.assertEqual(pair_volumes(20000, 0), (20000, 0))
        self.assertEqual(pair_volumes(20000, 64), (20000, 20000))
        self.assertEqual(pair_volumes(20000, 128), (0, 20000))

    def test_width_direction_and_all_mono_pans_roundtrip(self) -> None:
        for width in range(129):
            left, right = pair_pans(width)
            self.assertEqual(left + right, 128)
            self.assertEqual(pair_width(left, right), width)
        self.assertEqual(pair_width(64, 64), 64)
        self.assertEqual(pair_pans(128), (0, 128))
        self.assertEqual(pair_pans(0), (128, 0))
        for left in range(129):
            for right in range(129):
                self.assertEqual(
                    pair_pans(pair_width(left, right), pair_pan(left, right)),
                    (left, right),
                )


if __name__ == "__main__":
    unittest.main()
