#ifndef MT7601U_EVIDENCE_H
#define MT7601U_EVIDENCE_H
#include <stdint.h>
#ifndef INDEPENDENT_LOGGING
#define INDEPENDENT_LOGGING 1
#endif
#define MT7601U_EVIDENCE_MAGIC 0x4d544447u /* MTDG */
enum mt7601u_evidence_type {
	MT_EE_BLOCK = 1, MT_EE_RESULT, MT_EE_IDENTITY, MT_EE_CONFIG,
	MT_EE_PHY, MT_EE_NIC, MT_EE_TSSI, MT_EE_CHANNEL, MT_EE_RATE,
	MT_RF_READ, MT_CAL, MT_IQ, MT_IQ_RX, MT_BW, MT_DPD, MT_TSSI,
	MT_RUNTIME_EVENT = 18,
};
enum mt7601u_log_event {
	MT_EVENT_BSP_READY = 1,
	MT_EVENT_USB_SUSPEND,
	MT_EVENT_USB_RESUME,
	MT_EVENT_UNKNOWN_COMMAND,
	MT_EVENT_FIRMWARE_READY,
};
void mt7601u_log_event(enum mt7601u_log_event event, uint32_t first, uint32_t second);
void mt7601u_evidence_reset(void);
void mt7601u_evidence_emit(unsigned type, const uint32_t *words, unsigned count);
#if INDEPENDENT_LOGGING
#define MT7601U_REPORT(type, format, ...) mt7601u_log_printf(format, __VA_ARGS__)
#else
/* The format and its string literal never enter the quiet target. */
#define MT7601U_REPORT(type, format, ...) do { \
	const uint32_t evidence_words[] = { __VA_ARGS__ }; \
	mt7601u_evidence_emit(type, evidence_words, \
		sizeof(evidence_words) / sizeof(evidence_words[0])); \
} while (0)
#endif
#endif
