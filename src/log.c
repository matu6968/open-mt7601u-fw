#include "mt7601u_runtime.h"
#include "mt7601u_log_ring.h"

static volatile uint32_t *log_ring_header(void)
{
	return (volatile uint32_t *)(uintptr_t)MT7601U_LOG_RING_BASE;
}

#if INDEPENDENT_LOGGING
static void log_ring_put(uint8_t value)
{
	volatile uint32_t *header = log_ring_header();
	volatile uint8_t *data =
		(volatile uint8_t *)(uintptr_t)MT7601U_LOG_RING_DATA;
	uint32_t total = header[1];

	data[total & MT7601U_LOG_RING_MASK] = value;
	if (total >= MT7601U_LOG_RING_CAPACITY)
		header[2]++;
	/* Publish the cursor after the character and overflow count. */
	header[1] = total + 1u;
}

#endif
void mt7601u_log_ring_reset(void)
{
	volatile uint32_t *header = log_ring_header();

#if INDEPENDENT_LOGGING
	header[0] = MT7601U_LOG_RING_MAGIC;
#else
	header[0] = MT7601U_EVIDENCE_MAGIC;
	mt7601u_evidence_reset();
#endif
	header[1] = 0u;
	header[2] = 0u;
	header[3] = MT7601U_LOG_RING_CAPACITY;
}

#if INDEPENDENT_LOGGING
static void log_putc(uint8_t value)
{
	/* FUN_69AC in Imilab expands LF to CRLF before calling FUN_4AEC. */
	if (value == '\n')
		log_ring_put('\r');
	log_ring_put(value);
}

static void log_string(const char *text)
{
	if (text == (const char *)0)
		text = "(null)";
	while (*text != '\0')
		log_putc((uint8_t)*text++);
}

static void log_uint(uint32_t value, uint32_t base, uint32_t width,
		int zero_pad, int upper)
{
	/* UINT32_MAX needs ten decimal digits; hexadecimal needs eight. */
	char digits[10];
	uint32_t count = 0u;
	uint32_t i;
	const char *alphabet = upper ? "0123456789ABCDEF" :
		"0123456789abcdef";

	if (value == 0u)
		digits[count++] = '0';
	else
		while (value != 0u) {
			digits[count++] = alphabet[value % base];
			value /= base;
		}

	while (count < width) {
		log_putc((uint8_t)(zero_pad ? '0' : ' '));
		width--;
	}
	for (i = count; i != 0u; i--)
		log_putc((uint8_t)digits[i - 1u]);
}

static void log_signed(int32_t value, uint32_t width, int zero_pad)
{
	uint32_t magnitude;

	if (value < 0) {
		log_putc('-');
		magnitude = (uint32_t)(-(value + 1)) + 1u;
		if (width != 0u)
			width--;
	} else {
		magnitude = (uint32_t)value;
	}
	log_uint(magnitude, 10u, width, zero_pad, 0);
}

/* Small formatter for the conversions used by Imilab's diagnostic strings.
 * It intentionally has no dependency on libc or a heap. */
void mt7601u_log_printf(const char *format, ...)
{
	__builtin_va_list ap;

	__builtin_va_start(ap, format);
	while (*format != '\0') {
		uint32_t width = 0u;
		int zero_pad = 0;
		char conversion;

		if (*format != '%') {
			log_putc((uint8_t)*format++);
			continue;
		}
		format++;
		if (*format == '%') {
			log_putc('%');
			format++;
			continue;
		}
		if (*format == '0') {
			zero_pad = 1;
			format++;
		}
		while (*format >= '0' && *format <= '9')
			width = width * 10u + (uint32_t)(*format++ - '0');
		conversion = *format++;
		switch (conversion) {
		case 'c':
			log_putc((uint8_t)__builtin_va_arg(ap, int));
			break;
		case 's':
			log_string(__builtin_va_arg(ap, const char *));
			break;
		case 'd':
			log_signed(__builtin_va_arg(ap, int32_t), width, zero_pad);
			break;
		case 'u':
			log_uint(__builtin_va_arg(ap, uint32_t), 10u, width,
				zero_pad, 0);
			break;
		case 'x':
			log_uint(__builtin_va_arg(ap, uint32_t), 16u, width,
				zero_pad, 0);
			break;
		case 'X':
			log_uint(__builtin_va_arg(ap, uint32_t), 16u, width,
				zero_pad, 1);
			break;
		default:
			log_putc('%');
			if (zero_pad)
				log_putc('0');
			log_putc((uint8_t)conversion);
			break;
		}
	}
	__builtin_va_end(ap);
}
#endif

__attribute__((section(".events")))
void mt7601u_log_event(enum mt7601u_log_event event, uint32_t first, uint32_t second)
{
#if INDEPENDENT_LOGGING
	switch (event) {
	case MT_EVENT_BSP_READY:
		mt7601u_log_printf("BSP Init done.\n");
		break;
	case MT_EVENT_USB_SUSPEND:
		mt7601u_log_printf("Suspend! count=%u\n", first);
		break;
	case MT_EVENT_USB_RESUME:
		mt7601u_log_printf("ResumeIRQ count=%u\n", first);
		break;
	case MT_EVENT_UNKNOWN_COMMAND:
		mt7601u_log_printf("Unknown command:%u, len:%u\n", first, second);
		break;
	case MT_EVENT_FIRMWARE_READY:
		mt7601u_log_printf("Firmware ready: COM_REG0=%u, USB reads=%u\n", first, second);
		break;
	}
#else
	const uint32_t words[] = { (uint32_t)event, first, second };

	mt7601u_evidence_emit(MT_RUNTIME_EVENT, words, sizeof(words) / sizeof(words[0]));
#endif
}
