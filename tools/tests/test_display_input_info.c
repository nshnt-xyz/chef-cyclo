/* Exercise the actual command with mocked evdev syscalls, no device access. */
#include <assert.h>
#include <stdarg.h>
#define main fbtouch_main
#include "../fbtouch.c"
#undef main

static int opens, closes, queries, fail_open, fail_bits, fail_axis;

int __wrap_open(const char *path, int flags, ...)
{
	assert(strcmp(path, "/dev/input/event7") == 0);
	assert(flags == (O_RDONLY | O_NONBLOCK | O_CLOEXEC));
	opens++;
	if (fail_open) { errno = EACCES; return -1; }
	return 123;
}

int __wrap_close(int fd)
{
	assert(fd == 123);
	closes++;
	return 0;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *out;
	assert(fd == 123);
	va_start(ap, request);
	out = va_arg(ap, void *);
	va_end(ap);
	queries++;
	if (_IOC_NR(request) == _IOC_NR(EVIOCGBIT(EV_ABS, 0))) {
		unsigned long *bits = out;
		assert(_IOC_DIR(request) == _IOC_READ);
		if (fail_bits) { errno = ENOTTY; return -1; }
		bits[ABS_MT_POSITION_X / (8 * sizeof(long))] |= 1UL << (ABS_MT_POSITION_X % (8 * sizeof(long)));
		bits[ABS_MT_TRACKING_ID / (8 * sizeof(long))] |= 1UL << (ABS_MT_TRACKING_ID % (8 * sizeof(long)));
		return 0;
	}
	assert(request == EVIOCGABS(ABS_MT_POSITION_X) || request == EVIOCGABS(ABS_MT_TRACKING_ID));
	if (fail_axis && request == EVIOCGABS(ABS_MT_POSITION_X)) { errno = EIO; return -1; }
	*(struct input_absinfo *)out = (struct input_absinfo){ .minimum = -1, .maximum = 720,
		.fuzz = 2, .flat = 3, .resolution = 4 };
	return 0;
}

int main(void)
{
	FILE *capture = tmpfile();
	char output[512] = {0};
	int saved_stdout = dup(STDOUT_FILENO);
	assert(capture && saved_stdout >= 0);
	assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
	assert(cmd_input_info(NULL) == 64 && opens == 0);
	assert(cmd_input_info("/dev/input/event7") == 0);
	fflush(stdout);
	rewind(capture);
	assert(fread(output, 1, sizeof(output) - 1, capture) > 0);
	assert(strstr(output, "ABS 0x35: min=-1 max=720 fuzz=2 flat=3 resolution=4"));
	assert(strstr(output, "ABS 0x39:"));
	assert(opens == 1 && closes == 1 && queries == 3);
	fail_axis = 1;
	assert(cmd_input_info("/dev/input/event7") == 1);
	assert(queries == 6 && closes == 2); /* still queries next axis */
	fail_bits = 1;
	assert(cmd_input_info("/dev/input/event7") == 1 && closes == 3);
	fail_open = 1;
	assert(cmd_input_info("/dev/input/event7") == 1 && closes == 3);
	fflush(stdout);
	assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
	/* Wrapped close deliberately only accepts the evdev descriptor. */
	fclose(capture);
	puts("test-display-input-info: passed");
	return 0;
}
