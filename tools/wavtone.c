/*
 * wavtone - writes a deterministic PCM S16_LE sine-wave WAV file
 * (chef-cyclo).
 *
 * initramfs/usr/bin/speaker-test-tone needs a test tone tinyplay can push
 * straight to the TAS2560 speaker path; tinyalsa's tinyplay only reads WAV
 * files (no synth mode), so this writes one, to /run, at the sample
 * rate/channel count the TERT_MI2S_RX backend actually runs
 * (kernel/sound/soc/msm/sdm660-common.c:233 -- 48 kHz, S16_LE, 2 ch by
 * default; both -r and -c are overridable in case that ever changes).
 *
 * Every channel carries the identical sample (mono content, replicated),
 * so playback is the same tone out of the one speaker regardless of -c.
 * The waveform is a plain sin(2*pi*f*t) scaled to the requested peak
 * dBFS -- deterministic for a given (rate, channels, freq, duration,
 * amplitude): no randomness, no host-clock dependence.
 *
 * The ADSP's TAS2560 speaker-protection algorithm (TAS2560_ALGO_FF_MODULE)
 * has no calibration data loaded on this device (no ACDB loader), so an
 * uncalibrated full-scale tone is a real risk to the speaker; -a refuses
 * any amplitude louder than -6 dBFS outright rather than silently clamping
 * it, both here and in dbfs_to_amplitude() so a host test can assert the
 * refusal without ever asking the codec to play anything.
 *
 * Usage: wavtone -f HZ -d SEC -a DBFS [-r RATE] [-c CHANNELS] -o FILE
 *   -f  tone frequency in Hz (default 1000)
 *   -d  duration in seconds (default 1)
 *   -a  peak amplitude in dBFS, must be <= -6 (default -20)
 *   -r  sample rate in Hz (default 48000)
 *   -c  channel count (default 2)
 *   -o  output WAV file (required)
 * Exits 0 on success, 1 on a usage/argument/write error.
 *
 * The pure parts (amplitude-from-dBFS with its refusal above -6 dBFS,
 * frame-count rounding, the 44-byte canonical header, and the sample
 * generator) are separated from main() and covered by
 * tools/tests/test_wavtone.c, which includes this file with
 * WAVTONE_NO_MAIN.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WAVTONE_MAX_DBFS (-6.0)
#define WAVTONE_DEFAULT_FREQ 1000.0
#define WAVTONE_DEFAULT_DURATION 1.0
#define WAVTONE_DEFAULT_DBFS (-20.0)
#define WAVTONE_DEFAULT_RATE 48000u
#define WAVTONE_DEFAULT_CHANNELS 2u

/* Peak amplitude (0..32767) for a full-scale-relative dBFS value, or -1 if
 * dbfs is above WAVTONE_MAX_DBFS (refused, never clamped). 0 dBFS = 32767
 * (INT16_MAX, not 32768 -- the waveform must never reach -32768, which
 * would make the negative half-cycle one LSB louder than the positive
 * one). */
static int32_t dbfs_to_amplitude(double dbfs)
{
	double amp;

	if (dbfs > WAVTONE_MAX_DBFS)
		return -1;

	amp = 32767.0 * pow(10.0, dbfs / 20.0);
	if (amp < 0.0)
		amp = 0.0;
	if (amp > 32767.0)
		amp = 32767.0;
	return (int32_t)(amp + 0.5);
}

/* Number of sample frames for `duration` seconds at `rate` Hz, rounded to
 * the nearest frame. Returns 0 if rate or duration is non-positive. */
static uint32_t wavtone_frame_count(uint32_t rate, double duration)
{
	if (rate == 0 || duration <= 0.0)
		return 0;
	return (uint32_t)(rate * duration + 0.5);
}

/* Writes the canonical 44-byte PCM WAV header for `frames` frames of
 * `channels`-channel S16_LE audio at `rate` Hz. Returns 0 on success. */
static int write_wav_header(FILE *f, uint32_t rate, uint16_t channels, uint32_t frames)
{
	uint16_t bits_per_sample = 16;
	uint16_t block_align = channels * (bits_per_sample / 8);
	uint32_t byte_rate = rate * block_align;
	uint32_t data_bytes = frames * block_align;
	uint32_t riff_size = 36 + data_bytes;
	uint16_t audio_format = 1; /* PCM */
	uint32_t fmt_chunk_size = 16;
	unsigned char hdr[44];

	memcpy(hdr + 0, "RIFF", 4);
	hdr[4] = riff_size & 0xff;
	hdr[5] = (riff_size >> 8) & 0xff;
	hdr[6] = (riff_size >> 16) & 0xff;
	hdr[7] = (riff_size >> 24) & 0xff;
	memcpy(hdr + 8, "WAVE", 4);
	memcpy(hdr + 12, "fmt ", 4);
	hdr[16] = fmt_chunk_size & 0xff;
	hdr[17] = (fmt_chunk_size >> 8) & 0xff;
	hdr[18] = (fmt_chunk_size >> 16) & 0xff;
	hdr[19] = (fmt_chunk_size >> 24) & 0xff;
	hdr[20] = audio_format & 0xff;
	hdr[21] = (audio_format >> 8) & 0xff;
	hdr[22] = channels & 0xff;
	hdr[23] = (channels >> 8) & 0xff;
	hdr[24] = rate & 0xff;
	hdr[25] = (rate >> 8) & 0xff;
	hdr[26] = (rate >> 16) & 0xff;
	hdr[27] = (rate >> 24) & 0xff;
	hdr[28] = byte_rate & 0xff;
	hdr[29] = (byte_rate >> 8) & 0xff;
	hdr[30] = (byte_rate >> 16) & 0xff;
	hdr[31] = (byte_rate >> 24) & 0xff;
	hdr[32] = block_align & 0xff;
	hdr[33] = (block_align >> 8) & 0xff;
	hdr[34] = bits_per_sample & 0xff;
	hdr[35] = (bits_per_sample >> 8) & 0xff;
	memcpy(hdr + 36, "data", 4);
	hdr[40] = data_bytes & 0xff;
	hdr[41] = (data_bytes >> 8) & 0xff;
	hdr[42] = (data_bytes >> 16) & 0xff;
	hdr[43] = (data_bytes >> 24) & 0xff;

	return fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr) ? 0 : -1;
}

/* Writes `frames` frames of a sin(2*pi*freq*t) tone at `amplitude` peak,
 * `channels`-channel S16_LE, every channel carrying the identical sample.
 * Returns 0 on success. */
static int write_wav_samples(FILE *f, uint32_t rate, uint16_t channels,
			      double freq, uint32_t frames, int32_t amplitude)
{
	uint32_t i;
	uint16_t ch;

	for (i = 0; i < frames; i++) {
		double t = (double)i / (double)rate;
		double s = sin(2.0 * M_PI * freq * t);
		int16_t sample = (int16_t)lround(s * (double)amplitude);
		unsigned char b[2];

		b[0] = (unsigned char)(sample & 0xff);
		b[1] = (unsigned char)((sample >> 8) & 0xff);
		for (ch = 0; ch < channels; ch++) {
			if (fwrite(b, 1, 2, f) != 2)
				return -1;
		}
	}
	return 0;
}

static int write_wav_tone(FILE *f, uint32_t rate, uint16_t channels,
			   double freq, double duration, int32_t amplitude)
{
	uint32_t frames = wavtone_frame_count(rate, duration);

	if (write_wav_header(f, rate, channels, frames) != 0)
		return -1;
	return write_wav_samples(f, rate, channels, freq, frames, amplitude);
}

#ifndef WAVTONE_NO_MAIN

static void usage(void)
{
	fprintf(stderr,
		"usage: wavtone -f HZ -d SEC -a DBFS [-r RATE] [-c CHANNELS] -o FILE\n"
		"  -f  tone frequency in Hz (default %.0f)\n"
		"  -d  duration in seconds (default %.0f)\n"
		"  -a  peak amplitude in dBFS, must be <= %.0f (default %.0f)\n"
		"  -r  sample rate in Hz (default %u)\n"
		"  -c  channel count (default %u)\n"
		"  -o  output WAV file (required)\n",
		WAVTONE_DEFAULT_FREQ, WAVTONE_DEFAULT_DURATION, WAVTONE_MAX_DBFS,
		WAVTONE_DEFAULT_DBFS, WAVTONE_DEFAULT_RATE, WAVTONE_DEFAULT_CHANNELS);
}

int main(int argc, char **argv)
{
	double freq = WAVTONE_DEFAULT_FREQ;
	double duration = WAVTONE_DEFAULT_DURATION;
	double dbfs = WAVTONE_DEFAULT_DBFS;
	uint32_t rate = WAVTONE_DEFAULT_RATE;
	uint16_t channels = WAVTONE_DEFAULT_CHANNELS;
	const char *outpath = NULL;
	int32_t amplitude;
	int opt;
	FILE *f;

	while ((opt = getopt(argc, argv, "f:d:a:r:c:o:h")) != -1) {
		switch (opt) {
		case 'f': freq = atof(optarg); break;
		case 'd': duration = atof(optarg); break;
		case 'a': dbfs = atof(optarg); break;
		case 'r': rate = (uint32_t)strtoul(optarg, NULL, 10); break;
		case 'c': channels = (uint16_t)strtoul(optarg, NULL, 10); break;
		case 'o': outpath = optarg; break;
		default: usage(); return opt == 'h' ? 0 : 1;
		}
	}
	if (optind != argc || !outpath) {
		usage();
		return 1;
	}
	if (freq <= 0.0 || duration <= 0.0 || rate == 0 || channels == 0) {
		fprintf(stderr, "wavtone: -f/-d/-r/-c must be positive\n");
		return 1;
	}
	amplitude = dbfs_to_amplitude(dbfs);
	if (amplitude < 0) {
		fprintf(stderr, "wavtone: refusing amplitude %.1f dBFS (louder than %.0f dBFS)\n",
			dbfs, WAVTONE_MAX_DBFS);
		return 1;
	}

	f = fopen(outpath, "wb");
	if (!f) {
		fprintf(stderr, "wavtone: %s: %s\n", outpath, strerror(errno));
		return 1;
	}
	if (write_wav_tone(f, rate, channels, freq, duration, amplitude) != 0) {
		fprintf(stderr, "wavtone: write error: %s\n", strerror(errno));
		fclose(f);
		return 1;
	}
	fclose(f);
	fprintf(stderr, "wavtone: wrote %s (%.0f Hz, %.2fs, %.1f dBFS, %u Hz, %u ch)\n",
		outpath, freq, duration, dbfs, rate, channels);
	return 0;
}

#endif /* WAVTONE_NO_MAIN */
