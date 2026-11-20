#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$#" -ne 4 ]; then
	echo "usage: $0 CARD BASE_CAPTURE_CHANNELS HIGH_PLAYBACK_CHANNELS HIGH_CAPTURE_CHANNELS" >&2
	exit 2
fi

card=$1
base_capture=$2
high_playback=$3
high_capture=$4
case "$card" in
	''|*[!A-Za-z0-9_-]*) echo "invalid ALSA card ID" >&2; exit 2 ;;
esac
for channels in "$base_capture" "$high_playback" "$high_capture"; do
	case "$channels" in
		''|*[!0-9]*|0) echo "invalid channel count: $channels" >&2; exit 2 ;;
	esac
done

device="hw:$card"
proc="/proc/asound/$card"
play_pid=
capture_pid=
restore=0

cleanup()
{
	status=$?
	trap - EXIT INT TERM
	if [ -n "$capture_pid" ]; then
		kill "$capture_pid" 2>/dev/null || true
		wait "$capture_pid" 2>/dev/null || true
	fi
	if [ -n "$play_pid" ]; then
		kill "$play_pid" 2>/dev/null || true
		wait "$play_pid" 2>/dev/null || true
	fi
	if [ "$restore" -eq 1 ]; then
		if ! arecord -q -D "$device" -t raw -c "$base_capture" -f S32_LE \
			-r 48000 --period-size=256 --buffer-size=768 -d 1 /dev/null; then
			echo "could not restore 48 kHz" >&2
			status=1
		fi
	fi
	exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if ! grep -Fq 'Sample rate: 48000 Hz' "$proc/motu424" ||
   ! grep -Fq 'Engine: idle' "$proc/motu424" ||
   [ "$(cat "$proc/pcm0p/sub0/hw_params")" != closed ] ||
   [ "$(cat "$proc/pcm0c/sub0/hw_params")" != closed ]; then
	echo "card must be idle at 48 kHz with both PCM streams closed" >&2
	exit 1
fi

restore=1
aplay -q -D "$device" -t raw -c "$high_playback" -f S32_LE \
	-r 96000 --period-size=256 --buffer-size=768 -d 4 /dev/zero &
play_pid=$!

ready=0
i=0
while [ "$i" -lt 800 ]; do
	if grep -Fq 'Sample rate: 96000 Hz' "$proc/motu424" &&
	   grep -Fq 'Engine: audio' "$proc/motu424"; then
		ready=1
		break
	fi
	if ! kill -0 "$play_pid" 2>/dev/null; then
		echo "playback exited before the audio engine started" >&2
		exit 1
	fi
	i=$((i + 1))
	sleep 0.01
done
if [ "$ready" -ne 1 ]; then
	echo "timed out waiting for the audio engine" >&2
	exit 1
fi

sleep 0.1
play_state=$(awk '/^state:/ { print $2; exit }' "$proc/pcm0p/sub0/status")
if [ "$play_state" != SETUP ]; then
	echo "inconclusive: playback was not observed during its settle wait" >&2
	exit 1
fi
arecord -q -D "$device" -t raw -c "$high_capture" -f S32_LE \
	-r 96000 --period-size=256 --buffer-size=768 -s 2048 /dev/null &
capture_pid=$!
sleep 0.25
if ! kill -0 "$capture_pid" 2>/dev/null; then
	if ! wait "$capture_pid"; then
		capture_pid=
		echo "capture failed" >&2
		exit 1
	fi
	capture_pid=
	echo "capture finished while playback was still settling" >&2
	exit 1
fi
echo "short capture did not finish during playback prepare"

if ! wait "$capture_pid"; then
	capture_pid=
	echo "capture failed" >&2
	exit 1
fi
capture_pid=
if ! wait "$play_pid"; then
	play_pid=
	echo "playback failed" >&2
	exit 1
fi
play_pid=
echo "concurrent PCM settle test passed"
