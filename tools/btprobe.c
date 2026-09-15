/*
 * Tiny libc-free helper for early WCN3990 bring-up.
 *
 *   btprobe power 0|1
 *   btprobe write /dev/ttyHS0 HEX
 *   btprobe transact /dev/ttyHS0 HEX
 *   btprobe download /dev/ttyHS0 TLV_FILE
 *   btprobe baud /dev/ttyHS0 RATE 0|1
 *   btprobe attach /dev/ttyHS0 RATE
 *   btprobe hciup INDEX
 *   btprobe lescan INDEX
 *
 * "transact" writes the bytes, then prints everything received for two
 * seconds as hexadecimal.  UART framing and baud rate are set with busybox
 * stty by bt-bringup.
 */

typedef unsigned long usize;

#define AT_FDCWD       -100
#define O_RDWR         2
#define O_NONBLOCK     04000
#define BT_CMD_PWR_CTRL 0xbfad
#define TCGETS2         0x802c542a
#define TCSETS2         0x402c542b
#define TIOCSETD        0x5423
#define HCIUARTSETPROTO 0x400455c8
#define HCIDEVUP        0x400448c9

#define CBAUD           0x0000100f
#define BOTHER          0x00001000
#define CSIZE           0x00000030
#define CS8             0x00000030
#define CSTOPB          0x00000040
#define CREAD           0x00000080
#define PARENB          0x00000100
#define CLOCAL          0x00000800
#define CRTSCTS         0x80000000

#define SYS_IOCTL      29
#define SYS_OPENAT     56
#define SYS_CLOSE      57
#define SYS_READ       63
#define SYS_WRITE      64
#define SYS_NANOSLEEP  101
#define SYS_SOCKET     198
#define SYS_BIND       200
#define SYS_SETSOCKOPT 208
#define SYS_EXIT       93

struct timespec {
	long tv_sec;
	long tv_nsec;
};

struct termios2 {
	unsigned int c_iflag;
	unsigned int c_oflag;
	unsigned int c_cflag;
	unsigned int c_lflag;
	unsigned char c_line;
	unsigned char c_cc[19];
	unsigned int c_ispeed;
	unsigned int c_ospeed;
};

struct sockaddr_hci {
	unsigned short family;
	unsigned short dev;
	unsigned short channel;
};

struct hci_filter {
	unsigned int type_mask;
	unsigned int event_mask[2];
	unsigned short opcode;
	unsigned short pad;
};

static long syscall1(long nr, long a)
{
	register long x0 __asm__("x0") = a;
	register long x8 __asm__("x8") = nr;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8) : "memory");
	return x0;
}

static long syscall2(long nr, long a, long b)
{
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x8 __asm__("x8") = nr;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
	return x0;
}

static long syscall3(long nr, long a, long b, long c)
{
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x8 __asm__("x8") = nr;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
	return x0;
}

static long syscall4(long nr, long a, long b, long c, long d)
{
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x8 __asm__("x8") = nr;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
	return x0;
}

static long syscall5(long nr, long a, long b, long c, long d, long e)
{
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x8 __asm__("x8") = nr;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
			 "r"(x4), "r"(x8) : "memory");
	return x0;
}

static usize slen(const char *s)
{
	usize n = 0;
	while (s[n])
		n++;
	return n;
}

static int same(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void puts2(const char *s)
{
	syscall3(SYS_WRITE, 2, (long)s, slen(s));
}

static void put_number(long value)
{
	char buf[24];
	int pos = sizeof(buf), negative = value < 0;
	unsigned long n = negative ? (unsigned long)-value : (unsigned long)value;
	buf[--pos] = '\n';
	do {
		buf[--pos] = (char)('0' + n % 10);
		n /= 10;
	} while (n);
	if (negative)
		buf[--pos] = '-';
	syscall3(SYS_WRITE, 2, (long)(buf + pos), sizeof(buf) - pos);
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static unsigned int decimal(const char *s)
{
	unsigned int value = 0;
	while (*s >= '0' && *s <= '9')
		value = value * 10 + (unsigned int)(*s++ - '0');
	return *s ? 0 : value;
}

static int decode_hex(const char *s, unsigned char *out, int max)
{
	int hi = -1, n = 0, v;
	while (*s) {
		v = hexval(*s++);
		if (v < 0)
			continue;
		if (hi < 0) {
			hi = v;
		} else {
			if (n == max)
				return -1;
			out[n++] = (unsigned char)((hi << 4) | v);
			hi = -1;
		}
	}
	return hi < 0 ? n : -1;
}

static void print_hex(const unsigned char *buf, int len)
{
	static const char digits[] = "0123456789abcdef";
	char out[3 * 256 + 1];
	int i, p = 0;
	for (i = 0; i < len; i++) {
		out[p++] = digits[buf[i] >> 4];
		out[p++] = digits[buf[i] & 15];
		out[p++] = i == len - 1 ? '\n' : ' ';
	}
	syscall3(SYS_WRITE, 1, (long)out, p);
}

static void delay_10ms(void)
{
	struct timespec delay = { 0, 10000000 };
	syscall2(SYS_NANOSLEEP, (long)&delay, 0);
}

static void delay_25ms(void)
{
	/* A full 249-byte H4/TLV packet takes about 22 ms at 115200 baud. */
	struct timespec delay = { 0, 25000000 };
	syscall2(SYS_NANOSLEEP, (long)&delay, 0);
}

static int write_all(long fd, const unsigned char *buf, int len)
{
	int done = 0, retries = 0;
	long ret;
	while (done < len && retries < 500) {
		ret = syscall3(SYS_WRITE, fd, (long)(buf + done), len - done);
		if (ret > 0) {
			done += (int)ret;
			retries = 0;
		} else {
			retries++;
			delay_10ms();
		}
	}
	return done == len ? 0 : 1;
}

/* Wait for the QCA vendor response 04 ff 03 00 04 00. */
static int wait_tlv_ack(long fd)
{
	unsigned char buf[512];
	int used = 0, tries, i, saw_wake = 0;
	long ret;
	for (tries = 0; tries < 500; tries++) {
		ret = syscall3(SYS_READ, fd, (long)(buf + used), sizeof(buf) - used);
		if (ret > 0) {
			int first = used;
			used += (int)ret;
			/* Firmware may start IBS before NVM is loaded.  Acknowledge
			 * controller wake indications so it can deliver the result. */
			for (i = first; i < used; i++) {
				if (buf[i] == 0xfd) {
					static const unsigned char ack = 0xfc;
					saw_wake = 1;
					write_all(fd, &ack, 1);
				}
			}
			for (i = 0; i + 5 < used; i++) {
				if (buf[i] == 0x04 && buf[i + 1] == 0xff &&
				    buf[i + 3] == 0x00 && buf[i + 4] == 0x04) {
					print_hex(buf, used);
					return buf[i + 5] == 0 ? 0 : 1;
				}
			}
			if (used == (int)sizeof(buf))
				used = 0;
		}
		delay_10ms();
	}
	if (used)
		print_hex(buf, used);
	if (saw_wake)
		return 2;
	puts2("btprobe: timed out waiting for TLV acknowledgement\n");
	return 1;
}

static unsigned char firmware[131072];

/* Disable IBS/deep sleep in the NVM copy in RAM. */
static void make_nvm_diagnostic(unsigned char *data, int len)
{
	int pos = 4;
	if (len >= 8 && data[0] == 4)
		pos = 8; /* unified NVM has an extra four-byte header */
	while (pos + 12 <= len) {
		int tag = data[pos] | (data[pos + 1] << 8);
		int size = data[pos + 2] | (data[pos + 3] << 8);
		if (pos + 12 + size > len)
			break;
		if (tag == 17 && size >= 3) {
			data[pos + 12] &= 0x7f;
		}
		if (tag == 27 && size >= 1)
			data[pos + 12] &= 0xfe;
		pos += 12 + size;
	}
}

static int download(const char *path, const char *filename)
{
	unsigned char packet[1 + 3 + 2 + 243];
	long file, fd, ret;
	int len = 0, pos = 0, seg, mode;

	file = syscall4(SYS_OPENAT, AT_FDCWD, (long)filename, 0, 0);
	if (file < 0) {
		puts2("btprobe: cannot open firmware file\n");
		return 1;
	}
	while (len < (int)sizeof(firmware)) {
		ret = syscall3(SYS_READ, file, (long)(firmware + len),
			       sizeof(firmware) - len);
		if (ret <= 0)
			break;
		len += (int)ret;
	}
	syscall1(SYS_CLOSE, file);
	if (len < 5 || len == (int)sizeof(firmware)) {
		puts2("btprobe: invalid or oversized firmware file\n");
		return 1;
	}

	mode = firmware[0] == 1 && len > 14 ? firmware[14] : 0;
	if (firmware[0] == 4 || firmware[0] == 2)
		make_nvm_diagnostic(firmware, len);

	fd = syscall4(SYS_OPENAT, AT_FDCWD, (long)path,
		      O_RDWR | O_NONBLOCK, 0);
	if (fd < 0) {
		puts2("btprobe: cannot open UART\n");
		return 1;
	}
	while (pos < len) {
		seg = len - pos;
		if (seg > 243)
			seg = 243;
		packet[0] = 0x01; /* H4 command packet */
		packet[1] = 0x00;
		packet[2] = 0xfc; /* EDL_PATCH_CMD_OPCODE */
		packet[3] = (unsigned char)(seg + 2);
		packet[4] = 0x1e; /* EDL_PATCH_TLV_REQ_CMD */
		packet[5] = (unsigned char)seg;
		for (ret = 0; ret < seg; ret++)
			packet[6 + ret] = firmware[pos + ret];
		if (write_all(fd, packet, seg + 6)) {
			puts2("btprobe: firmware segment write failed\n");
			syscall1(SYS_CLOSE, fd);
			return 1;
		}
		/* This controller suppresses every patch response in mode 3,
		 * including the last one. NVM acknowledges every segment. */
		if (mode != 3) {
			int ack = wait_tlv_ack(fd);
			if (ack) {
				syscall1(SYS_CLOSE, fd);
				return 1;
			}
		}
		if (mode == 3)
			delay_25ms();
		pos += seg;
	}
	syscall1(SYS_CLOSE, fd);
	return 0;
}

static int power(const char *value)
{
	long fd = syscall4(SYS_OPENAT, AT_FDCWD, (long)"/dev/btpower", O_RDWR, 0);
	long ret;
	if (fd < 0) {
		puts2("btprobe: cannot open /dev/btpower\n");
		return 1;
	}
	ret = syscall3(SYS_IOCTL, fd, BT_CMD_PWR_CTRL, value[0] == '1');
	syscall1(SYS_CLOSE, fd);
	if (ret < 0) {
		puts2("btprobe: BT_CMD_PWR_CTRL failed\n");
		return 1;
	}
	return 0;
}

static int set_baud(const char *path, const char *rate_string,
		    const char *flow_string)
{
	struct termios2 tio;
	unsigned int rate = decimal(rate_string);
	long fd, ret;
	if (!rate) {
		puts2("btprobe: invalid baud rate\n");
		return 1;
	}
	fd = syscall4(SYS_OPENAT, AT_FDCWD, (long)path, O_RDWR, 0);
	if (fd < 0) {
		puts2("btprobe: cannot open UART\n");
		return 1;
	}
	ret = syscall3(SYS_IOCTL, fd, TCGETS2, (long)&tio);
	if (ret < 0) {
		puts2("btprobe: TCGETS2 failed\n");
		syscall1(SYS_CLOSE, fd);
		return 1;
	}
	tio.c_iflag = 0;
	tio.c_oflag = 0;
	tio.c_lflag = 0;
	tio.c_cflag &= ~(CBAUD | CSIZE | CSTOPB | PARENB | CRTSCTS);
	tio.c_cflag |= BOTHER | CS8 | CREAD | CLOCAL;
	if (flow_string[0] == '1')
		tio.c_cflag |= CRTSCTS;
	tio.c_ispeed = rate;
	tio.c_ospeed = rate;
	ret = syscall3(SYS_IOCTL, fd, TCSETS2, (long)&tio);
	syscall1(SYS_CLOSE, fd);
	if (ret < 0) {
		puts2("btprobe: TCSETS2 failed\n");
		return 1;
	}
	return 0;
}

static int attach_h4(const char *path, const char *rate_string)
{
	struct timespec delay = { 1, 0 };
	int ldisc = 15; /* N_HCI */
	long fd;
	if (set_baud(path, rate_string, "1"))
		return 1;
	fd = syscall4(SYS_OPENAT, AT_FDCWD, (long)path, O_RDWR, 0);
	if (fd < 0) {
		puts2("btprobe: cannot open UART for attach\n");
		return 1;
	}
	if (syscall3(SYS_IOCTL, fd, TIOCSETD, (long)&ldisc) < 0) {
		puts2("btprobe: TIOCSETD N_HCI failed\n");
		return 1;
	}
	if (syscall3(SYS_IOCTL, fd, HCIUARTSETPROTO, 0) < 0) {
		puts2("btprobe: HCIUARTSETPROTO H4 failed\n");
		return 1;
	}
	puts2("btprobe: H4 attached; keeping UART open\n");
	for (;;)
		syscall2(SYS_NANOSLEEP, (long)&delay, 0);
}

static int hci_up(const char *index_string)
{
	unsigned int index = decimal(index_string);
	long ret, fd = syscall3(SYS_SOCKET, 31, 3, 1); /* AF_BLUETOOTH/SOCK_RAW/HCI */
	if (fd < 0) {
		puts2("btprobe: cannot open raw HCI socket\n");
		return 1;
	}
	ret = syscall3(SYS_IOCTL, fd, HCIDEVUP, index);
	if (ret == -114) { /* EALREADY */
		syscall1(SYS_CLOSE, fd);
		return 0;
	}
	if (ret < 0) {
		puts2("btprobe: HCIDEVUP failed, errno ");
		put_number(-ret);
		syscall1(SYS_CLOSE, fd);
		return 1;
	}
	syscall1(SYS_CLOSE, fd);
	return 0;
}

static int le_scan(const char *index_string)
{
	static const unsigned char params[] = {
		0x01, 0x0b, 0x20, 0x07, 0x01, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00
	};
	static const unsigned char enable[] = { 0x01, 0x0c, 0x20, 0x02, 0x01, 0x00 };
	static const unsigned char disable[] = { 0x01, 0x0c, 0x20, 0x02, 0x00, 0x00 };
	struct sockaddr_hci addr;
	struct hci_filter filter;
	unsigned char rx[512];
	long fd, ret;
	int i;

	addr.family = 31; /* AF_BLUETOOTH */
	addr.dev = (unsigned short)decimal(index_string);
	addr.channel = 0; /* HCI_CHANNEL_RAW */
	filter.type_mask = 1U << 4; /* HCI_EVENT_PKT */
	filter.event_mask[0] = 0xffffffffU;
	filter.event_mask[1] = 0xffffffffU;
	filter.opcode = 0;
	filter.pad = 0;

	fd = syscall3(SYS_SOCKET, 31, 3 | O_NONBLOCK, 1);
	if (fd < 0 || syscall3(SYS_BIND, fd, (long)&addr, sizeof(addr)) < 0) {
		puts2("btprobe: cannot bind raw HCI socket\n");
		return 1;
	}
	if (syscall5(SYS_SETSOCKOPT, fd, 0, 2, (long)&filter,
		     sizeof(filter)) < 0) {
		puts2("btprobe: cannot set HCI event filter\n");
		return 1;
	}
	if (write_all(fd, params, sizeof(params)))
		return 1;
	for (i = 0; i < 20; i++) {
		ret = syscall3(SYS_READ, fd, (long)rx, sizeof(rx));
		if (ret > 0)
			print_hex(rx, (int)ret);
		delay_10ms();
	}
	if (write_all(fd, enable, sizeof(enable)))
		return 1;
	puts2("btprobe: scanning for 10 seconds\n");
	for (i = 0; i < 1000; i++) {
		ret = syscall3(SYS_READ, fd, (long)rx, sizeof(rx));
		if (ret > 0)
			print_hex(rx, (int)ret);
		delay_10ms();
	}
	write_all(fd, disable, sizeof(disable));
	syscall1(SYS_CLOSE, fd);
	return 0;
}

static int uart(const char *path, const char *hex, int receive)
{
	unsigned char tx[256], rx[256];
	struct timespec delay = { 0, 10000000 };
	long fd, ret;
	int len, tries;

	len = decode_hex(hex, tx, sizeof(tx));
	if (len <= 0) {
		puts2("btprobe: invalid or empty hex string\n");
		return 1;
	}
	fd = syscall4(SYS_OPENAT, AT_FDCWD, (long)path,
		      O_RDWR | O_NONBLOCK, 0);
	if (fd < 0) {
		puts2("btprobe: cannot open UART\n");
		return 1;
	}
	ret = write_all(fd, tx, len);
	if (ret) {
		puts2("btprobe: UART write failed\n");
		syscall1(SYS_CLOSE, fd);
		return 1;
	}
	if (receive) {
		for (tries = 0; tries < 200; tries++) {
			ret = syscall3(SYS_READ, fd, (long)rx, sizeof(rx));
			if (ret > 0)
				print_hex(rx, (int)ret);
			syscall2(SYS_NANOSLEEP, (long)&delay, 0);
		}
	}
	syscall1(SYS_CLOSE, fd);
	return 0;
}

static int program_main(int argc, char **argv)
{
	if (argc == 3 && same(argv[1], "power"))
		return power(argv[2]);
	if (argc == 4 && same(argv[1], "write"))
		return uart(argv[2], argv[3], 0);
	if (argc == 4 && same(argv[1], "transact"))
		return uart(argv[2], argv[3], 1);
	if (argc == 4 && same(argv[1], "download"))
		return download(argv[2], argv[3]);
	if (argc == 5 && same(argv[1], "baud"))
		return set_baud(argv[2], argv[3], argv[4]);
	if (argc == 4 && same(argv[1], "attach"))
		return attach_h4(argv[2], argv[3]);
	if (argc == 3 && same(argv[1], "hciup"))
		return hci_up(argv[2]);
	if (argc == 3 && same(argv[1], "lescan"))
		return le_scan(argv[2]);
	puts2("usage: btprobe power 0|1 | write UART HEX | transact UART HEX | download UART FILE | baud UART RATE FLOW | attach UART RATE | hciup INDEX | lescan INDEX\n");
	return 2;
}

__attribute__((used)) void start(unsigned long *stack)
{
	int argc = (int)stack[0];
	char **argv = (char **)&stack[1];
	syscall1(SYS_EXIT, program_main(argc, argv));
	for (;;)
		;
}

__asm__(".global _start\n"
	"_start:\n"
	"mov x0, sp\n"
	"bl start\n");
