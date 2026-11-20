"""Conversion between a linked pair's two routes and its stereo controls.

The two route volumes and pans are the mixer state. Gain, balance, width and pan
are another description of the same state:

* gain is the louder of the two volumes;
* balance attenuates the other side by ``1 - offset ** 2``, where ``offset``
  is the distance from the centre (the law of the driver's balance control);
* pan is the midpoint of the two pans;
* width is 64 plus half their signed separation (128 is normal stereo).

Converting volumes to gain and balance and back returns the same volumes.
The reverse direction rounds to whole volume steps, so a caller that needs
the exact balance back passes the value it last set as ``hint``.
"""

import math


CENTER = 64
PAN_MAX = 128


def pair_width(left_pan: int, right_pan: int) -> float:
    return CENTER + (right_pan - left_pan) / 2


def pair_pan(left_pan: int, right_pan: int) -> float:
    return (left_pan + right_pan) / 2


def pair_pans(width: float, pan: float = CENTER) -> tuple[int, int]:
    """Translate both pans or change their separation without clipping one side."""
    separation = width - CENTER
    headroom = CENTER - abs(separation)
    pan = max(CENTER - headroom, min(CENTER + headroom, pan))
    return round(pan - separation), round(pan + separation)


def pair_volumes(gain: int, balance: float) -> tuple[int, int]:
    offset = (balance - CENTER) / CENTER
    quiet = round(gain * (1 - offset * offset))
    return (quiet, gain) if balance > CENTER else (gain, quiet)


def pair_balance(
    left_volume: int, right_volume: int, hint: float | None = None
) -> float:
    gain = max(left_volume, right_volume)
    if hint is not None and pair_volumes(gain, hint) == (left_volume, right_volume):
        return hint
    if left_volume == right_volume:
        return CENTER
    offset = math.sqrt(1 - min(left_volume, right_volume) / gain)
    if right_volume > left_volume:
        return CENTER + CENTER * offset
    return CENTER - CENTER * offset
