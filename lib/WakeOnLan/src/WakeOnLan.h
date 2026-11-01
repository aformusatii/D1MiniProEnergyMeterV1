#ifndef _WakeOnLan_H_
#define _WakeOnLan_H_

#include <stddef.h>
#include <stdint.h>

#define WOL_MAC_LEN     6
#define WOL_PACKET_LEN  102

/*
 * Wake-on-LAN magic packet: 6 x 0xFF followed by the target MAC repeated 16 times.
 * No Arduino dependencies (host-tested), sending is done by the caller over UDP.
 */
class WakeOnLan {

	public:
		// Parses "48:2A:E3:20:D6:81" (':' or '-' separators, any case)
		static bool parseMac(const char *text, uint8_t mac[WOL_MAC_LEN]);
		static void formatMac(const uint8_t mac[WOL_MAC_LEN], char *buf, size_t size);
		static void buildPacket(const uint8_t mac[WOL_MAC_LEN], uint8_t packet[WOL_PACKET_LEN]);
};

#endif /* _WakeOnLan_H_ */
