#ifndef MT7601U_PACKET_H
#define MT7601U_PACKET_H
#include <stdint.h>
#define MT7601U_PACKET_DIAG_BASE 0xc900u
#define MT7601U_PACKET_DIAG_END 0xc9c0u
struct mt7601u_runtime;
void mt7601u_packet_init(struct mt7601u_runtime *runtime);
void mt7601u_packet_service(struct mt7601u_runtime *runtime, uint32_t events);
void mt7601u_packet_receive(struct mt7601u_runtime *runtime);
#endif
