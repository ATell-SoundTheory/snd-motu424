// SPDX-License-Identifier: GPL-2.0-only
/*
 * Prints what an audio server sees when it opens the card: the constraint
 * space per rate, the flags PipeWire and JACK look at, which channel and
 * period requests are accepted, and the granularity of the stream position.
 * Only capture streams are started; nothing is played.
 *
 * cc -O2 -Wall -Wextra -Werror tools/pcm_caps.c -lasound -o pcm_caps
 * pcm_caps [DEVICE]
 */
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const unsigned int rates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static const char *dir_name(snd_pcm_stream_t dir)
{
	return dir == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture";
}

/* hw_params restricted to the rate, or NULL if the rate is refused */
static int restrict_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw,
			 unsigned int rate)
{
	if (snd_pcm_hw_params_any(pcm, hw) < 0)
		return -1;
	if (snd_pcm_hw_params_set_access(pcm, hw,
					 SND_PCM_ACCESS_MMAP_INTERLEAVED) < 0)
		return -1;
	if (snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE) < 0)
		return -1;
	return snd_pcm_hw_params_set_rate(pcm, hw, rate, 0);
}

static void print_space(const char *dev, snd_pcm_stream_t dir,
			unsigned int rate)
{
	snd_pcm_hw_params_t *hw, *t;
	snd_pcm_uframes_t pmin, pmax, bmin, bmax, frames;
	unsigned int cmin, cmax, nmin, nmax, c;
	static const unsigned int try_ch[] = { 1, 2, 8, 24, 32, 64 };
	static const unsigned int try_periods[] = { 2, 3, 4 };
	static const snd_pcm_uframes_t try_period[] = { 32, 48, 128, 256, 512, 1024 };
	snd_pcm_t *pcm;
	size_t i;
	int dirn = 0;

	if (snd_pcm_open(&pcm, dev, dir, SND_PCM_NONBLOCK) < 0) {
		printf("%s %u: open failed\n", dir_name(dir), rate);
		return;
	}
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_alloca(&t);
	if (restrict_rate(pcm, hw, rate) < 0) {
		printf("%s %u: rate refused\n", dir_name(dir), rate);
		snd_pcm_close(pcm);
		return;
	}
	snd_pcm_hw_params_get_channels_min(hw, &cmin);
	snd_pcm_hw_params_get_channels_max(hw, &cmax);
	snd_pcm_hw_params_get_period_size_min(hw, &pmin, &dirn);
	snd_pcm_hw_params_get_period_size_max(hw, &pmax, &dirn);
	snd_pcm_hw_params_get_periods_min(hw, &nmin, &dirn);
	snd_pcm_hw_params_get_periods_max(hw, &nmax, &dirn);
	snd_pcm_hw_params_get_buffer_size_min(hw, &bmin);
	snd_pcm_hw_params_get_buffer_size_max(hw, &bmax);
	printf("%s %u: channels %u-%u period %lu-%lu periods %u-%u buffer %lu-%lu"
	       " batch %d disable-period-wakeup %d monotonic %d sync-start %d\n",
	       dir_name(dir), rate, cmin, cmax, pmin, pmax, nmin, nmax, bmin, bmax,
	       snd_pcm_hw_params_is_batch(hw),
	       snd_pcm_hw_params_can_disable_period_wakeup(hw),
	       snd_pcm_hw_params_is_monotonic(hw),
	       snd_pcm_hw_params_can_sync_start(hw));

	printf("  channels accepted:");
	for (i = 0; i < sizeof(try_ch) / sizeof(try_ch[0]); i++) {
		snd_pcm_hw_params_copy(t, hw);
		printf(" %u:%s", try_ch[i],
		       snd_pcm_hw_params_set_channels(pcm, t, try_ch[i]) < 0 ?
		       "no" : "yes");
	}
	snd_pcm_hw_params_copy(t, hw);
	c = 64;
	snd_pcm_hw_params_set_channels_near(pcm, t, &c);
	printf(" near(64)->%u\n", c);

	printf("  periods accepted:");
	for (i = 0; i < sizeof(try_periods) / sizeof(try_periods[0]); i++) {
		snd_pcm_hw_params_copy(t, hw);
		printf(" %u:%s", try_periods[i],
		       snd_pcm_hw_params_set_periods(pcm, t, try_periods[i], 0) < 0 ?
		       "no" : "yes");
	}
	printf("\n  period sizes accepted:");
	for (i = 0; i < sizeof(try_period) / sizeof(try_period[0]); i++) {
		snd_pcm_hw_params_copy(t, hw);
		printf(" %lu:%s", try_period[i],
		       snd_pcm_hw_params_set_period_size(pcm, t, try_period[i], 0) < 0 ?
		       "no" : "yes");
	}
	/* what a timer-scheduled server asks for: the largest buffer */
	snd_pcm_hw_params_copy(t, hw);
	frames = bmax;
	snd_pcm_hw_params_set_buffer_size_near(pcm, t, &frames);
	printf("\n  buffer near(max)=%lu", frames);
	snd_pcm_hw_params_copy(t, hw);
	frames = 8192;
	if (snd_pcm_hw_params_set_buffer_size_near(pcm, t, &frames) < 0)
		printf(" near(8192)=refused\n");
	else
		printf(" near(8192)=%lu\n", frames);
	snd_pcm_close(pcm);
}

/*
 * Run a capture stream for SECONDS and sample snd_pcm_avail() every 250 us:
 * the smallest step of the position is what the driver reports between
 * interrupts (PipeWire's timer scheduling assumes it is sample-exact).
 */
static int pointer_steps(const char *dev, unsigned int rate,
			 snd_pcm_uframes_t period, double seconds)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_uframes_t step_min = (snd_pcm_uframes_t)-1, step_max = 0;
	snd_pcm_sframes_t last = 0, avail;
	unsigned int ch, steps = 0, samples = 0;
	snd_pcm_t *pcm;
	double end;
	int err;

	if (snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK) < 0)
		return -1;
	snd_pcm_hw_params_alloca(&hw);
	if (restrict_rate(pcm, hw, rate) < 0) {
		snd_pcm_close(pcm);
		return -1;
	}
	snd_pcm_hw_params_get_channels_max(hw, &ch);
	if (snd_pcm_hw_params_set_channels(pcm, hw, ch) < 0 ||
	    snd_pcm_hw_params_set_period_size(pcm, hw, period, 0) < 0 ||
	    snd_pcm_hw_params_set_periods(pcm, hw, 3, 0) < 0 ||
	    (err = snd_pcm_hw_params(pcm, hw)) < 0) {
		printf("pointer %u/%lu: hw_params refused\n", rate, period);
		snd_pcm_close(pcm);
		return -1;
	}
	snd_pcm_start(pcm);
	end = now() + seconds;
	while (now() < end) {
		struct timespec ts = { 0, 250000 };

		avail = snd_pcm_avail(pcm);
		if (avail < 0) {
			printf("pointer %u/%lu: %s\n", rate, period, snd_strerror(avail));
			break;
		}
		if (avail > last) {
			snd_pcm_uframes_t d = avail - last;

			if (d < step_min)
				step_min = d;
			if (d > step_max)
				step_max = d;
			steps++;
		}
		samples++;
		/* consume whole periods only, keeping the step measurement exact */
		if ((snd_pcm_uframes_t)avail >= 2 * period) {
			snd_pcm_forward(pcm, avail);
			avail = 0;
		}
		last = avail;
		nanosleep(&ts, NULL);
	}
	printf("pointer %u Hz period %lu (%u ch): %u position steps in %u polls,"
	       " step %lu-%lu frames\n", rate, period, ch, steps, samples,
	       steps ? step_min : 0, step_max);
	snd_pcm_close(pcm);
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "hw:M424,0";
	size_t i;

	for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		print_space(dev, SND_PCM_STREAM_PLAYBACK, rates[i]);
		print_space(dev, SND_PCM_STREAM_CAPTURE, rates[i]);
	}
	pointer_steps(dev, 48000, 256, 2);
	pointer_steps(dev, 48000, 1024, 2);
	return 0;
}
