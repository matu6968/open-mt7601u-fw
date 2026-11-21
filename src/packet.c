#include "mt7601u_packet.h"
#include "mt7601u_runtime.h"
#include "mt7601u_wow.h"
#if defined(__NDS32__) && !defined(__NDS32_EL__)
#error "The packet profile requires a little-endian C data ABI (PSW.BE=0)"
#endif
#ifndef PACKET_READ
#define PACKET_READ(off) WLAN_REG(off)
#define PACKET_WRITE(off, value) (WLAN_REG(off) = (value))
#endif
/* Native words in MMIO; descriptors and RXWI remain explicit little endian. */
static uint32_t counters[12];
static uint8_t active_logged;
static uint8_t pending, pending_output;
static uint32_t le32(const volatile uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}
static unsigned le16(const volatile uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }
static void put32(uint8_t *p, uint32_t v)
{
	unsigned i;
	for (i = 0; i < 4; i++)
		p[i] = v >> (i * 8);
}
static void put16(uint8_t *p, unsigned v)
{
	p[0] = v;
	p[1] = v >> 8;
}
static void fence(void)
{
	__asm__ volatile("" ::: "memory");
#ifdef __NDS32__
	__builtin_nds32_dsb();
#endif
}
static void snapshot(struct mt7601u_runtime *r)
{
	unsigned i;
	MMIO32(MT7601U_PACKET_DIAG_BASE) = 0x4d54504bu;
	MMIO32(MT7601U_PACKET_DIAG_BASE + 4) = 1;
	for (i = 0; i < 12; i++)
		MMIO32(MT7601U_PACKET_DIAG_BASE + 8 + i * 4) = counters[i];
	MMIO32(MT7601U_PACKET_DIAG_BASE + 56) = r->fault;
	MMIO32(MT7601U_PACKET_DIAG_BASE + 60) = PACKET_READ(0x1004);
	MMIO32(MT7601U_PACKET_DIAG_BASE + 64) = PACKET_READ(0x804);
	MMIO32(MT7601U_PACKET_DIAG_BASE + 68) = PACKET_READ(0x80c);
	MMIO32(MT7601U_PACKET_DIAG_BASE + 72) = PACKET_READ(0x9c4);
	MMIO32(MT7601U_PACKET_DIAG_BASE + 76) = r->fce.count_commands;
	MMIO32(MT7601U_PACKET_DIAG_BASE + 80) = r->fce.count_responses;
	MMIO32(MT7601U_PACKET_DIAG_BASE + 84) = mt7601u_trap_code;
	MMIO32(MT7601U_PACKET_DIAG_BASE + 88) = mt7601u_trap_cause;
	volatile uint8_t *out = r->fce.cmd_resp.desc[pending_output];
	for (i = 0; i < 4; i++)
		MMIO32(MT7601U_PACKET_DIAG_BASE + 128 + i * 4) = le32(out + i * 4);
	MMIO32(MT7601U_PACKET_DIAG_BASE + 144) = PACKET_READ(0x990);
	MMIO32(MT7601U_PACKET_DIAG_BASE + 148) = PACKET_READ(0x9b0);
	MMIO32(MT7601U_PACKET_DIAG_BASE + 152) = FCE_CMD_CPU_IDX8;
	MMIO32(MT7601U_PACKET_DIAG_BASE + 156) = FCE_CMD_CPU_IDX_HOST8;
	MMIO32(MT7601U_PACKET_DIAG_BASE + 160) = FCE_TX1_CPU_IDX8;
	MMIO32(MT7601U_PACKET_DIAG_BASE + 164) = FCE_TX1_IDX8;
	MMIO32(MT7601U_PACKET_DIAG_BASE + 168) = pending | (pending_output << 8);
	MMIO32(MT7601U_PACKET_DIAG_BASE + 172) = r->usb_ep0_requests;
	MMIO32(MT7601U_PACKET_DIAG_BASE + 176) = r->usb_setup_captures;
	MMIO32(MT7601U_PACKET_DIAG_BASE + 180) = r->usb_map_events;
	MMIO32(MT7601U_PACKET_DIAG_BASE + 184) = r->usb_reset_events;
	MMIO32(MT7601U_PACKET_DIAG_BASE + 188) = r->last_usb_endpoint_status;
}
void mt7601u_packet_init(struct mt7601u_runtime *r)
{
	unsigned j;
	for (j = 0; j < 12; j++)
		counters[j] = 0;
	active_logged = pending = pending_output = 0;
	unsigned i;
	for (i = 0; i < 48; i++)
		MMIO32(MT7601U_PACKET_DIAG_BASE + i * 4) = 0;
	/* Test ordinary SRAM word/byte aliasing on the actual CPU. Native-width
	 * MMIO readback alone cannot detect a compiler/PSW data ABI mismatch. */
	volatile union {
		uint32_t word;
		uint8_t bytes[4];
	} abi;
	abi.word = 0x12345678u;
	if (abi.bytes[0] != 0x78 || abi.bytes[1] != 0x56 || abi.bytes[2] != 0x34 ||
	    abi.bytes[3] != 0x12)
		r->fault = 0xfce60003u;
	/* Imilab 0x448: normal host packet routing, before Linux's MAC tables.
	 * Linux owns MAC enables, RX filters, TXWI, keys and host USB DMA setup. */
	PACKET_WRITE(0x810, 0);
	PACKET_WRITE(0x80c, PACKET_READ(0x80c) | 0x03000000u);
	if (PACKET_READ(0x810) || (PACKET_READ(0x80c) & 0x03000000u) != 0x03000000u)
		r->fault = 0xfce60001u;
	counters[0] = !r->fault;
	snapshot(r);
	mt7601u_log_printf("packet: init status=%08x routing=%08x\n", r->fault,
			   PACKET_READ(0x80c));
}
/* Exact instruction transcription of 0x3d10/0x3e0c, with bounded work.
 * 900/90c select one of four error queues; their head and successor pages
 * feed 874/86c. Finish one packet before yielding; never guess page links. */
static int release_errors(unsigned receive)
{
	unsigned reg = receive ? 0x900 : 0x90c, done = 0;
	for (; done < 8; done++) {
		uint32_t pending = PACKET_READ(reg);
		unsigned mask = (pending >> 16) & 15, queue, head, tail, pages, i;
		if (!mask)
			return 0;
		for (queue = 0; queue < 4 && !(mask & (1u << queue)); queue++)
			;
		PACKET_WRITE(reg, queue);
		head = (PACKET_READ(reg) >> 8) & 127;
		tail = head;
		PACKET_WRITE(0x874, (head << 8) | head);
		uint32_t control = PACKET_READ(0x888), length = PACKET_READ(0x880);
		/* Pool page IDs and page count are seven bits. Reject overflow before
		 * releasing anything; stale/repeated errors cannot hang EP0 forever. */
		if (length > 127u * 128u - 4u) {
			counters[8]++;
			return 1;
		}
		pages = (length + 4u + 127u) / 128u;
		if (!pages) {
			counters[8]++;
			return 1;
		}
		PACKET_WRITE(0x888, (control & ~0x38u) | (receive ? 0x28u : 0x20u));
		for (i = 0; i + 1 < pages; i++) {
			PACKET_WRITE(0x86c, tail << 8);
			tail = PACKET_READ(0x86c) & 127;
		}
		uint32_t commit = (head << 24) | (tail << 16) | (pages << 8);
		if (receive) {
			PACKET_WRITE(0x904, commit | ((control >> 1) & 3));
			PACKET_WRITE(0x908, commit | 2);
		} else {
			PACKET_WRITE(0x910, commit | 0x18);
			PACKET_WRITE(0x914, commit | 2);
		}
		counters[receive ? 6 : 7]++;
		counters[9] += pages;
	}
	counters[10]++; /* More queued work: resume at the next foreground pass. */
	return 2;
}
void mt7601u_packet_service(struct mt7601u_runtime *r, uint32_t events)
{
	uint32_t ack = events & 0x00030003u;
	counters[1]++;
	if (events & (1u << 24)) {
		int status = release_errors(1);
		if (!status)
			ack |= 1u << 24;
		else if (status == 1)
			r->fault = 0xfce60002u;
	}
	if (events & (1u << 25)) {
		int status = release_errors(0);
		if (!status)
			ack |= 1u << 25;
		else if (status == 1)
			r->fault = 0xfce60002u;
	}
	if (ack)
		PACKET_WRITE(0x9e0, ack);
	if (!active_logged && (PACKET_READ(0x1004) & 12) == 12) {
		active_logged = 1;
		mt7601u_log_printf("packet: Linux MAC TX/RX enabled\n");
		snapshot(r);
	}
	/* Sparse snapshots retain counters without continuous diagnostic MMIO. */
	if (!(counters[1] & 0xfffu))
		snapshot(r);
}
void mt7601u_packet_receive(struct mt7601u_runtime *r)
{
	struct mt7601u_fce_state *s = &r->fce;
	unsigned budget;
	for (budget = 0; budget < 2; budget++) {
		unsigned index = s->idx_tx1, producer = FCE_TX1_CPU_IDX8 & 1;
		volatile uint8_t *d = s->tx1.desc[index];
		uint8_t *p = s->tx1_payload[index];
		if (pending) {
			volatile uint8_t *out = s->cmd_resp.desc[pending_output];
			/* FCE DDONE means it has copied the payload out of MCU SRAM. A
			 * command may already have reused that completed output
			 * descriptor; its changed pointer is also proof that
			 * command-ring ownership was returned. */
			uint32_t out_pointer = le32(out);
			uint32_t command_pointer =
			    (uint32_t)(uintptr_t)s->response_payload[pending_output];
			if (!(out[7] & 128) && out_pointer != command_pointer)
				break;
			pending = 0;
			counters[11] = 0;
			goto reclaim;
		}
		if (index == producer || !(d[7] & 128))
			break;
		fence();
		unsigned length = le16((const uint8_t *)d + 6) & 0x3fff;
		uint32_t info = le32((const uint8_t *)d + 12),
			 pointer = le32((const uint8_t *)d);
		MMIO32(MT7601U_PACKET_DIAG_BASE + 92) = length;
		MMIO32(MT7601U_PACKET_DIAG_BASE + 96) = info;
		MMIO32(MT7601U_PACKET_DIAG_BASE + 100) = pointer;
		MMIO32(MT7601U_PACKET_DIAG_BASE + 104) = le32(p);
		MMIO32(MT7601U_PACKET_DIAG_BASE + 108) = le32(p + 4);
		MMIO32(MT7601U_PACKET_DIAG_BASE + 112) = le32(p + 12);
		MMIO32(MT7601U_PACKET_DIAG_BASE + 116) = (uint32_t)(uintptr_t)p;
		MMIO32(MT7601U_PACKET_DIAG_BASE + 120) = producer;
		MMIO32(MT7601U_PACKET_DIAG_BASE + 124) = index;
		int valid = pointer == (uint32_t)(uintptr_t)p && length >= 38 &&
			    length <= MT7601U_FCE_TX1_BYTES && !(info & 0xc0000000u);
		if (valid) {
			uint32_t rxinfo = le32(p), rxctl = le32(p + 4),
				 rxzero = le32(p + 12);
			unsigned mpdu = (rxctl >> 16) & 4095,
				 pad = (rxinfo & (1u << 14)) ? 2 : 0;
			valid = mpdu >= 10 && mpdu + 28 + pad <= length;
			if (valid && mt7601u_wow_receive(p + 28, mpdu, rxinfo))
				goto reclaim;
			valid = valid && !(rxinfo & 7u) && !(rxzero & 0xffffff00u);
		}
		if (valid) {
			unsigned out = s->idx_resp, next = (out + 1) & 1,
				 aligned = (length + 3) & ~3u;
			if (next == (FCE_CMD_CPU_IDX_HOST8 & 1) ||
			    !(s->cmd_resp.desc[out][7] & 128)) {
				counters[5]++;
				break;
			}
			unsigned i;
			for (i = length; i < aligned; i++)
				p[i] = 0;
			uint8_t *desc = s->cmd_resp.desc[out];
			for (i = 0; i < 16; i++)
				desc[i] = 0;
			put32(desc, (uint32_t)(uintptr_t)p);
			put16(desc + 6, aligned | 0x4000);
			/* Packet type, host port, packet endpoint QSEL=0. Keep FCE
			 * checksum/ 802.11 metadata and the entire RXINFO/RXWI,
			 * including L2 padding. */
			put32(desc + 12, 0x18000000u | (info & 0x01ff0000u) | aligned);
			fence();
			s->idx_resp = next;
			FCE_CMD_CPU_IDX8 = next;
			counters[3]++;
			pending = 1;
			pending_output = out;
			counters[11] = 1;
			snapshot(r);
			/* Keep the receive descriptor and its payload until output
			 * completion. Firmware never returns this input buffer while
			 * USB DMA still owns it. */
			break;
		} else
			counters[4]++;
	reclaim:
		counters[2]++;
		unsigned i;
		for (i = 0; i < 16; i++)
			d[i] = 0;
		put32((uint8_t *)d, (uint32_t)(uintptr_t)p);
		put16((uint8_t *)d + 6, MT7601U_FCE_TX1_BYTES | 0x4000);
		fence();
		FCE_TX1_IDX8 = index;
		s->idx_tx1 = (index + 1) & 1;
		snapshot(r);
	}
}
