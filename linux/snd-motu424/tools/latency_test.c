// SPDX-License-Identifier: GPL-2.0-only
/*
 * Full-duplex analog-loopback test. Only the selected output carries a signal
 * (-40 dBFS peak); all other playback channels are silent.
 * MOTU424_START_DELAY_MS optionally delays the linked start after preparation.
 *
 * cc -O2 -Wall -Wextra -Werror tools/latency_test.c -lasound -lm -o latency_test
 * latency_test DEVICE RATE PERIOD BUFFER QUEUE_PERIODS SECONDS OUT_CH IN_CH OUT IN
 *
 * OUT/IN are zero-based PCM channel indices. Keep monitors muted.
 * For real-time scheduling start it with chrt, e.g. "chrt -f 70 latency_test ...".
 */
#include <alsa/asoundlib.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

struct event {
	size_t output, input;
};

static void check(int result, const char *operation)
{
	if (result < 0) {
		fprintf(stderr, "%s: %s\n", operation, snd_strerror(result));
		exit(1);
	}
}

static unsigned int number(const char *text, unsigned int min, unsigned int max)
{
	char *end;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || !*text || *end || value < min || value > max) {
		fprintf(stderr, "invalid numeric argument: %s\n", text);
		exit(1);
	}
	return value;
}

static double now(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
		perror("clock_gettime");
		exit(1);
	}
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int32_t signal_sample(size_t frame)
{
	uint32_t x = (uint32_t)(frame / 8) + 0x6d2b79f5;

	x ^= x >> 16;
	x *= 0x7feb352d;
	x ^= x >> 15;
	x *= 0x846ca68b;
	x ^= x >> 16;
	return x & 1 ? 21474836 : -21474836;
}

static double correlation(const int32_t *a, const int32_t *b, size_t count)
{
	double xy = 0, xx = 0, yy = 0, x = 0, y = 0, variance;
	size_t i;

	for (i = 0; i < count; i++) {
		double av = a[i] / 2147483648.0, bv = b[i] / 2147483648.0;

		x += av;
		y += bv;
		xy += av * bv;
		xx += av * av;
		yy += bv * bv;
	}
	variance = (xx - x * x / count) * (yy - y * y / count);
	return variance > 0 ? (xy - x * y / count) / sqrt(variance) : 0;
}

static size_t find_lag(const int32_t *sent, const int32_t *received,
		       size_t start, size_t count, size_t max_lag, double *score)
{
	size_t lag, best = 0;

	*score = 0;
	for (lag = 0; lag <= max_lag; lag++) {
		double r = correlation(sent + start - lag, received + start, count);

		if (fabs(r) > fabs(*score)) {
			*score = r;
			best = lag;
		}
	}
	return best;
}

static size_t bad_centers(const int32_t *sent, const int32_t *received,
			  size_t start, size_t count, size_t lag,
			  double polarity, double threshold)
{
	size_t i, bad = 0;

	/* Avoid converter-filter transients at the eight-frame chip edges. */
	for (i = start; i < start + count; i++) {
		size_t phase = (i - lag) % 8;
		double expected_sign = sent[i - lag] > 0 ? 1 : -1;

		if ((phase == 3 || phase == 4) &&
		    received[i] * polarity * expected_sign < threshold)
			bad++;
	}
	return bad;
}

static void configure(snd_pcm_t *pcm, unsigned int channels, unsigned int rate,
		       snd_pcm_uframes_t period, snd_pcm_uframes_t buffer,
		       snd_pcm_uframes_t avail_min)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	snd_pcm_uframes_t actual_period, actual_buffer;
	int dir = 0;

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_sw_params_alloca(&sw);
	check(snd_pcm_hw_params_any(pcm, hw), "hw_params_any");
	check(snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED), "access");
	check(snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE), "format");
	check(snd_pcm_hw_params_set_channels(pcm, hw, channels), "channels");
	check(snd_pcm_hw_params_set_rate(pcm, hw, rate, 0), "rate");
	check(snd_pcm_hw_params_set_period_size(pcm, hw, period, 0), "period");
	check(snd_pcm_hw_params_set_buffer_size(pcm, hw, buffer), "buffer");
	check(snd_pcm_hw_params(pcm, hw), "hw_params");
	check(snd_pcm_hw_params_get_period_size(hw, &actual_period, &dir), "get period");
	check(snd_pcm_hw_params_get_buffer_size(hw, &actual_buffer), "get buffer");
	if (actual_period != period || actual_buffer != buffer) {
		fprintf(stderr, "negotiated period/buffer differ from requested values\n");
		exit(1);
	}
	check(snd_pcm_sw_params_current(pcm, sw), "sw_params_current");
	check(snd_pcm_sw_params_set_avail_min(pcm, sw, avail_min), "avail_min");
	check(snd_pcm_sw_params_set_start_threshold(pcm, sw, buffer + 1), "start_threshold");
	check(snd_pcm_sw_params_set_stop_threshold(pcm, sw, buffer), "stop_threshold");
	check(snd_pcm_sw_params(pcm, sw), "sw_params");
	check(snd_pcm_prepare(pcm), "prepare");
}

int main(int argc, char **argv)
{
	snd_pcm_t *play, *capture;
	unsigned int rate, period, buffer, queue, seconds, out_ch, in_ch, out, in;
	unsigned int start_delay_ms = 0;
	size_t target, capacity, written = 0, captured = 0, i, lag, weak = 0, windows = 0;
	size_t nevents = 0, next_event, max_lag, start, bad;
	int32_t *tx, *rx, *sent, *received;
	struct event *events;
	struct pollfd *fds;
	int np, nc, n, result = 0;
	double begin, score, min_score = 1, min_rtt = 1e9, max_rtt = 0;
	double reference = 0;

	if (argc == 2 && !strcmp(argv[1], "--selftest")) {
		int32_t a[4096], b[4096] = { 0 };

		for (i = 0; i < 4096; i++)
			a[i] = signal_sample(i);
		for (i = 137; i < 4096; i++)
			b[i] = a[i - 137];
		assert(find_lag(a, b, 1024, 1024, 512, &score) == 137 && score > 0.999);
		assert(!bad_centers(a, b, 1024, 1024, 137, 1, 1000000));
		b[1028] = 0; /* (1028 - 137) % 8 == 3 */
		assert(bad_centers(a, b, 1024, 1024, 137, 1, 1000000) == 1);
		b[1028] = -a[1028 - 137];
		assert(bad_centers(a, b, 1024, 1024, 137, 1, 1000000) == 1);
		puts("latency analysis selftest: ok");
		return 0;
	}
	if (argc != 11) {
		fprintf(stderr, "usage: %s DEVICE RATE PERIOD BUFFER QUEUE_PERIODS SECONDS"
			" OUT_CH IN_CH OUT IN\n", argv[0]);
		return 1;
	}
	{
		const char *delay = getenv("MOTU424_START_DELAY_MS");

		if (delay)
			start_delay_ms = number(delay, 0, 60000);
	}
	rate = number(argv[2], 8000, 192000);
	period = number(argv[3], 16, 8192);
	buffer = number(argv[4], period, 65536);
	queue = number(argv[5], 1, buffer / period);
	seconds = number(argv[6], 3, 300);
	out_ch = number(argv[7], 1, 96);
	in_ch = number(argv[8], 1, 96);
	out = number(argv[9], 0, out_ch - 1);
	in = number(argv[10], 0, in_ch - 1);
	target = ((size_t)rate * seconds + period - 1) / period * period;
	capacity = target + 4 * buffer;
	tx = calloc((size_t)period * out_ch, sizeof(*tx));
	rx = calloc((size_t)period * in_ch, sizeof(*rx));
	sent = calloc(capacity, sizeof(*sent));
	received = calloc(capacity, sizeof(*received));
	events = calloc(seconds + 4, sizeof(*events));
	if (!tx || !rx || !sent || !received || !events) {
		fprintf(stderr, "sample allocation failed\n");
		return 1;
	}
	{
		struct sched_param sp = { 0 };
		int policy = sched_getscheduler(0);
		int locked;

		if (policy < 0 || sched_getparam(0, &sp) < 0) {
			perror("get scheduling parameters");
			return 1;
		}
		locked = mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
		if (!locked)
			fprintf(stderr, "mlockall: %s\n", strerror(errno));
		printf("sched_policy=%d priority=%d mlock=%s\n",
		       policy, sp.sched_priority, locked ? "yes" : "no");
	}
	check(snd_pcm_open(&play, argv[1], SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK), "open playback");
	check(snd_pcm_open(&capture, argv[1], SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK), "open capture");
	configure(play, out_ch, rate, period, buffer, buffer - queue * period + period);
	configure(capture, in_ch, rate, period, buffer, period);
	check(snd_pcm_link(play, capture), "link");
	np = snd_pcm_poll_descriptors_count(play);
	nc = snd_pcm_poll_descriptors_count(capture);
	check(np, "play poll count");
	check(nc, "capture poll count");
	if (!np || !nc) {
		fprintf(stderr, "PCM handles have no poll descriptors\n");
		return 1;
	}
	fds = calloc((size_t)np + nc, sizeof(*fds));
	if (!fds) {
		fprintf(stderr, "poll allocation failed\n");
		return 1;
	}
	check(snd_pcm_poll_descriptors(play, fds, np), "play poll descriptors");
	check(snd_pcm_poll_descriptors(capture, fds + np, nc), "capture poll descriptors");
	for (n = 0; n < (int)queue; n++) {
		for (i = 0; i < period; i++)
			sent[written + i] = tx[i * out_ch + out] = signal_sample(written + i);
		if (snd_pcm_writei(play, tx, period) != period) {
			fprintf(stderr, "initial playback write failed\n");
			return 1;
		}
		written += period;
	}
	next_event = rate;
	if (start_delay_ms) {
		printf("waiting %u ms before starting linked PCM\n", start_delay_ms);
		fflush(stdout);
		if (poll(NULL, 0, start_delay_ms) < 0) {
			perror("start delay");
			result = 1;
			goto stop;
		}
	}
	begin = now();
	check(snd_pcm_start(play), "start");
	printf("rate=%u period=%u buffer=%u queue=%u seconds=%u out=%u in=%u delay_ms=%u\n",
	       rate, period, buffer, queue, seconds, out, in, start_delay_ms);
	fflush(stdout);
	while (captured < target) {
		snd_pcm_sframes_t avail;

		if (now() - begin > seconds + 5) {
			fprintf(stderr, "stream progress timed out\n");
			result = 1;
			break;
		}
		avail = snd_pcm_avail_update(capture);
		if (avail < 0)
			check(avail, "capture avail (no xrun recovery)");
		while (avail >= period && captured < target) {
			snd_pcm_sframes_t frames = snd_pcm_readi(capture, rx, period);

			if (frames != period) {
				fprintf(stderr, "capture transfer: %ld (expected %u)\n", frames, period);
				result = 1;
				goto stop;
			}
			for (i = 0; i < period; i++)
				received[captured + i] = rx[i * in_ch + in];
			captured += period;
			avail -= period;
		}
		avail = snd_pcm_avail_update(play);
		if (avail < 0)
			check(avail, "playback avail (no xrun recovery)");
		while (avail >= buffer - queue * period + period) {
			snd_pcm_sframes_t frames;

			if (written + period > capacity) {
				fprintf(stderr, "playback ran ahead of capture\n");
				result = 1;
				goto stop;
			}
			if (captured >= next_event && nevents < seconds + 4) {
				events[nevents++] = (struct event){ written, captured - period };
				next_event += rate;
			}
			for (i = 0; i < period; i++)
				sent[written + i] = tx[i * out_ch + out] = signal_sample(written + i);
			frames = snd_pcm_writei(play, tx, period);
			if (frames != period) {
				fprintf(stderr, "playback transfer: %ld (expected %u)\n", frames, period);
				result = 1;
				goto stop;
			}
			written += period;
			avail -= period;
		}
		n = poll(fds, np + nc, 1000);
		if (n < 0) {
			perror("poll");
			result = 1;
			break;
		}
	}
stop:
	check(snd_pcm_drop(play), "drop");
	check(snd_pcm_unlink(play), "unlink");
	check(snd_pcm_close(capture), "close capture");
	check(snd_pcm_close(play), "close playback");
	if (result)
		return result;
	max_lag = buffer + 8 * period + 512;
	start = max_lag + rate;
	if (start + 2048 > captured) {
		fprintf(stderr, "capture too short for correlation analysis\n");
		return 1;
	}
	lag = find_lag(sent, received, start, 2048, max_lag, &score);
	for (i = start; i < start + 2048; i++)
		reference += fabs((double)received[i]) / 2048;
	bad = bad_centers(sent, received, start, captured - start, lag,
			  score < 0 ? -1 : 1, reference / 4);
	for (i = start; i + 512 <= captured; i += 512) {
		double r = fabs(correlation(sent + i - lag, received + i, 512));

		if (r < min_score)
			min_score = r;
		weak += r < 0.90;
		windows++;
	}
	for (i = 0; i < nevents; i++) {
		double rtt;

		if (events[i].output + lag >= captured)
			continue;
		rtt = (double)(events[i].output + lag - events[i].input) * 1000 / rate;
		if (rtt < min_rtt)
			min_rtt = rtt;
		if (rtt > max_rtt)
			max_rtt = rtt;
	}
	printf("frames=%zu xruns=0 lag=%zu correlation=%.6f min_window=%.6f"
	       " weak_windows=%zu/%zu bad_centers=%zu app_roundtrip_ms=%.3f..%.3f\n",
	       captured, lag, score, min_score, weak, windows, bad, min_rtt, max_rtt);
	free(fds);
	free(events);
	free(received);
	free(sent);
	free(rx);
	free(tx);
	return fabs(score) >= 0.90 && !weak && !bad && max_rtt > 0 ? 0 : 1;
}
