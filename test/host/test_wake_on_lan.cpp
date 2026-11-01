/*
 * Host test for lib/WakeOnLan (MAC parsing and magic packet layout).
 *
 *   g++ -std=gnu++17 -Wall -I lib/WakeOnLan/src test/host/test_wake_on_lan.cpp \
 *       lib/WakeOnLan/src/WakeOnLan.cpp -o .pio/test_wake_on_lan && .pio/test_wake_on_lan
 */
#include "WakeOnLan.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main() {
	uint8_t mac[WOL_MAC_LEN];
	const uint8_t t580[WOL_MAC_LEN] = {0x48, 0x2A, 0xE3, 0x20, 0xD6, 0x81};

	// --- parseMac ---
	CHECK(WakeOnLan::parseMac("48:2A:E3:20:D6:81", mac) && memcmp(mac, t580, WOL_MAC_LEN) == 0, "upper case, ':'");
	CHECK(WakeOnLan::parseMac("48-2a-e3-20-d6-81", mac) && memcmp(mac, t580, WOL_MAC_LEN) == 0, "lower case, '-'");
	CHECK(!WakeOnLan::parseMac("48:2A:E3:20:D6", mac), "too short");
	CHECK(!WakeOnLan::parseMac("48:2A:E3:20:D6:81:00", mac), "too long");
	CHECK(!WakeOnLan::parseMac("48:2A:E3:20:D6:8G", mac), "bad hex digit");
	CHECK(!WakeOnLan::parseMac("48.2A.E3.20.D6.81", mac), "bad separator");
	CHECK(!WakeOnLan::parseMac(nullptr, mac), "null");

	// --- formatMac ---
	char text[18];
	WakeOnLan::formatMac(t580, text, sizeof(text));
	CHECK(strcmp(text, "48:2A:E3:20:D6:81") == 0, "formatMac: %s", text);

	// --- buildPacket ---
	uint8_t packet[WOL_PACKET_LEN];
	memset(packet, 0, sizeof(packet));
	WakeOnLan::buildPacket(t580, packet);
	for (int i = 0; i < 6; i++) {
		CHECK(packet[i] == 0xFF, "sync byte %d", i);
	}
	for (int rep = 0; rep < 16; rep++) {
		CHECK(memcmp(packet + 6 + rep * 6, t580, WOL_MAC_LEN) == 0, "MAC repetition %d", rep);
	}

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("All WakeOnLan tests passed\n");
	return 0;
}
