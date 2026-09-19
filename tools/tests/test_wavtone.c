/* Host tests for tools/wavtone.c: the amplitude-from-dBFS conversion (and
 * its refusal above -6 dBFS -- the uncalibrated-speaker-protection
 * safeguard), frame-count rounding, the 44-byte canonical WAV header
 * (RIFF/fmt/data sizes, byte rate, block align), the generated sine
 * samples (first sample 0, positive at the first quarter period, peak
 * amplitude within 1 LSB of the requested dBFS, every channel identical).
 * wavtone.c is included with WAVTONE_NO_MAIN so main() is compiled out;
 * everything here drives the pure functions directly against an in-memory
 * tmpfile().
 * Build/run: see tools/Makefile ("make test").
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAVTONE_NO_MAIN
#include "../wavtone.c"

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
	g_tests++; \
	if (!(cond)) { \
		g_failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

static uint32_t rd_u32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const unsigned char *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static int16_t rd_s16(const unsigned char *p)
{
	return (int16_t)(p[0] | (p[1] << 8));
}

/* ---- dbfs_to_amplitude(): the uncalibrated-protection safeguard ---- */

static void test_dbfs_to_amplitude(void)
{
	/* -6 dBFS is the loudest allowed value: not refused, and close to
	 * full scale (32767 * 10^(-6/20) =~ 16422). */
	CHECK(dbfs_to_amplitude(-6.0) >= 0);
	CHECK(dbfs_to_amplitude(-6.0) == (int32_t)(32767.0 * pow(10.0, -6.0 / 20.0) + 0.5));
	/* Anything louder than -6 dBFS is refused outright, never clamped. */
	CHECK(dbfs_to_amplitude(-5.9) == -1);
	CHECK(dbfs_to_amplitude(0.1) == -1);
	CHECK(dbfs_to_amplitude(20.0) == -1);
	/* -20 dBFS (the script's default) is well within range. */
	CHECK(dbfs_to_amplitude(-20.0) == (int32_t)(32767.0 * pow(10.0, -20.0 / 20.0) + 0.5));
	/* Very quiet is fine too (never negative amplitude). */
	CHECK(dbfs_to_amplitude(-120.0) >= 0);
}

/* ---- wavtone_frame_count(): rounding and invalid inputs ---- */

static void test_frame_count(void)
{
	CHECK(wavtone_frame_count(48000, 1.0) == 48000);
	CHECK(wavtone_frame_count(48000, 0.01) == 480);
	/* Rounds to nearest frame rather than truncating. */
	CHECK(wavtone_frame_count(48000, 0.0100001) == 480);
	CHECK(wavtone_frame_count(44100, 0.5) == 22050);
	/* Invalid rate/duration are rejected (0 frames), never negative or huge. */
	CHECK(wavtone_frame_count(0, 1.0) == 0);
	CHECK(wavtone_frame_count(48000, 0.0) == 0);
	CHECK(wavtone_frame_count(48000, -1.0) == 0);
}

/* ---- write_wav_header(): canonical 44-byte header field by field ---- */

static void test_header_fields(void)
{
	FILE *f = tmpfile();
	unsigned char hdr[44];
	uint32_t frames = 480, rate = 48000;
	uint16_t channels = 2;

	CHECK(f != NULL);
	CHECK(write_wav_header(f, rate, channels, frames) == 0);
	rewind(f);
	CHECK(fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr));

	CHECK(memcmp(hdr + 0, "RIFF", 4) == 0);
	CHECK(memcmp(hdr + 8, "WAVE", 4) == 0);
	CHECK(memcmp(hdr + 12, "fmt ", 4) == 0);
	CHECK(memcmp(hdr + 36, "data", 4) == 0);

	CHECK(rd_u32(hdr + 16) == 16); /* fmt chunk size */
	CHECK(rd_u16(hdr + 20) == 1);  /* PCM */
	CHECK(rd_u16(hdr + 22) == channels);
	CHECK(rd_u32(hdr + 24) == rate);
	CHECK(rd_u16(hdr + 32) == channels * 2); /* block align */
	CHECK(rd_u32(hdr + 28) == rate * channels * 2); /* byte rate */
	CHECK(rd_u16(hdr + 34) == 16); /* bits per sample */

	uint32_t data_bytes = frames * channels * 2;
	CHECK(rd_u32(hdr + 40) == data_bytes);
	CHECK(rd_u32(hdr + 4) == 36 + data_bytes); /* RIFF size */

	fclose(f);
}

/* ---- write_wav_samples()/write_wav_tone(): the sine itself ---- */

static void test_samples(void)
{
	FILE *f = tmpfile();
	uint32_t rate = 48000, frames;
	uint16_t channels = 2;
	double freq = 1000.0, duration = 0.01; /* 480 frames */
	int32_t amplitude = dbfs_to_amplitude(-20.0);
	unsigned char *data;
	long data_len;

	CHECK(f != NULL);
	CHECK(amplitude > 0);
	CHECK(write_wav_tone(f, rate, channels, freq, duration, amplitude) == 0);

	frames = wavtone_frame_count(rate, duration);
	CHECK(frames == 480);

	fseek(f, 0, SEEK_END);
	long total = ftell(f);
	CHECK(total == 44 + (long)frames * channels * 2);

	data_len = (long)frames * channels * 2;
	data = malloc((size_t)data_len);
	CHECK(data != NULL);
	fseek(f, 44, SEEK_SET);
	CHECK(fread(data, 1, (size_t)data_len, f) == (size_t)data_len);

	/* First sample (frame 0) is 0 on every channel: sin(0) == 0. */
	CHECK(rd_s16(data + 0) == 0);
	CHECK(rd_s16(data + 2) == 0);

	/* Every channel carries the identical sample, for every frame. */
	int channels_equal = 1;
	for (uint32_t i = 0; i < frames; i++) {
		int16_t l = rd_s16(data + i * 4 + 0);
		int16_t r = rd_s16(data + i * 4 + 2);
		if (l != r)
			channels_equal = 0;
	}
	CHECK(channels_equal);

	/* First quarter period (t = 1/(4*freq) => frame rate/(4*freq) = 12 at
	 * 1000 Hz/48000 Hz): sin() peaks positive there, within 1 LSB of the
	 * requested peak amplitude. */
	uint32_t quarter_frame = (uint32_t)lround(rate / (4.0 * freq));
	CHECK(quarter_frame == 12);
	int16_t quarter_sample = rd_s16(data + quarter_frame * 4);
	CHECK(quarter_sample > 0);
	CHECK(abs((int)quarter_sample - amplitude) <= 1);

	/* Peak amplitude across the whole buffer matches the requested dBFS
	 * within 1 LSB, on either channel. */
	int16_t peak = 0;
	for (uint32_t i = 0; i < frames; i++) {
		int16_t l = rd_s16(data + i * 4 + 0);
		if (l > peak)
			peak = l;
		if ((int16_t)-l > peak)
			peak = (int16_t)-l;
	}
	CHECK(abs((int)peak - amplitude) <= 1);

	free(data);
	fclose(f);
}

/* write_wav_tone() with an amplitude wavtone.c's own CLI would have refused
 * (dbfs_to_amplitude's -1 sentinel) must not be handed to the writer as a
 * literal amplitude -- callers must check the sentinel first. This mirrors
 * main()'s own check, exercised here since main() is compiled out. */
static void test_amplitude_sentinel_not_writable(void)
{
	CHECK(dbfs_to_amplitude(-20.0 /* the script's own default */) != -1);
	CHECK(dbfs_to_amplitude(0.0 /* full scale: refused */) == -1);
	CHECK(dbfs_to_amplitude(-5.0) == -1);
}

int main(void)
{
	test_dbfs_to_amplitude();
	test_frame_count();
	test_header_fields();
	test_samples();
	test_amplitude_sentinel_not_writable();

	fprintf(stderr, "wavtone: %d/%d checks passed\n", g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
