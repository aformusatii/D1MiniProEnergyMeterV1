#include "WakeOnLan.h"

#include <stdio.h>
#include <string.h>

static int hexValue(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

bool WakeOnLan::parseMac(const char *text, uint8_t mac[WOL_MAC_LEN]) {
	if (text == nullptr || strlen(text) != 17) {
		return false;
	}

	for (int i = 0; i < WOL_MAC_LEN; i++) {
		const char *p = text + i * 3;
		int hi = hexValue(p[0]);
		int lo = hexValue(p[1]);
		if (hi < 0 || lo < 0) {
			return false;
		}
		if (i < WOL_MAC_LEN - 1 && p[2] != ':' && p[2] != '-') {
			return false;
		}
		mac[i] = (uint8_t) ((hi << 4) | lo);
	}

	return true;
}

void WakeOnLan::formatMac(const uint8_t mac[WOL_MAC_LEN], char *buf, size_t size) {
	snprintf(buf, size, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void WakeOnLan::buildPacket(const uint8_t mac[WOL_MAC_LEN], uint8_t packet[WOL_PACKET_LEN]) {
	memset(packet, 0xFF, WOL_MAC_LEN);
	for (int i = 1; i <= 16; i++) {
		memcpy(packet + i * WOL_MAC_LEN, mac, WOL_MAC_LEN);
	}
}
