#include "WiFiHelper.h"

WiFiHelper::WiFiHelper() {
	wifi_check_timer = 0;
	wiFiOk = false;
	wifi_last_status = WL_NO_SHIELD;

	status_led_timer = 0;
	ledInterval = STATUS_LED_NOT_CONNECTED;
	statusLEDPort = 0;
	useStatusLED = false;
	lastStatusLEDState = LOW;

	watchdogStatus = WATCHDOG_START;
	watchdogWiFiLostCounter = 0;
	watchdogWiFiWaitCounter = 0;
}

void WiFiHelper::begin(const char *ssid,const char *key, std::function<void(void)> scallback) {
	setup_callback = scallback;

	WiFi.mode(WIFI_STA);
	WiFi.begin(ssid, key);
}

void WiFiHelper::loop(unsigned long current_time) {

	blinkLedStatus(current_time);

	if ((current_time - wifi_check_timer) < WIFI_CHECK_INTERVAL_MS) {
		// not the time yet
		return;
	}

	wifi_check_timer = current_time;

	wl_status_t status = WiFi.status();

	// Watchdog monitors status of WIFI to apply reset when necessary
	watchdogLoop(status);

	// inform only if wifi status changes
	if (status != wifi_last_status) {
		Serial.println("WIFI Status: " + String(status));
		calculateLedStatusFrequency(status);
	}

	// Store last wifi status
	wifi_last_status = status;

	if (wiFiOk) {
		// setup complete exit
		return;
	}

	// kick off the setup if this is not complete and WIFI just connected
	if (status == WL_CONNECTED) {
		setup_callback();
		wiFiOk = true;
	}

}

bool WiFiHelper::configureStaticIp(uint8_t first_octet, uint8_t second_octet, uint8_t third_octet, uint8_t fourth_octet) {

	IPAddress localIP(first_octet, second_octet, third_octet, fourth_octet);
	IPAddress gateway(192, 168, 100, 1);
	IPAddress subnet(255, 255, 255, 0);
	IPAddress primaryDNS(1, 1, 1, 1);
	IPAddress secondaryDNS(8, 8, 8, 8);

	return WiFi.config(localIP, gateway, subnet, primaryDNS, secondaryDNS);
}

void WiFiHelper::setupLedStatus(uint8_t confStatusLEDPort) {
	useStatusLED = true;
	statusLEDPort = confStatusLEDPort;

	pinMode(confStatusLEDPort, OUTPUT);
}

void WiFiHelper::blinkLedStatus(unsigned long current_time) {
	if (!useStatusLED) {
		// do not use status LED
		return;
	}

	if (current_time < status_led_timer) {
		// not the time yet
		return;
	}

	// calculate next check
	status_led_timer = current_time + ledInterval;

	lastStatusLEDState = (lastStatusLEDState == LOW) ? HIGH : LOW;
	digitalWrite(statusLEDPort, lastStatusLEDState);
}

void WiFiHelper::calculateLedStatusFrequency(wl_status_t wl_status) {
	switch(wl_status) {
		case WL_CONNECTED:
			ledInterval = STATUS_LED_CONNECTED;
			return;

		case WL_CONNECT_FAILED:
		case WL_CONNECTION_LOST:
			ledInterval = STATUS_LED_CON_LOST;
			return;

		default:
			ledInterval = STATUS_LED_NOT_CONNECTED;
	}
}

void WiFiHelper::watchdogLoop(wl_status_t wl_status) {
	switch(watchdogStatus) {
		case WATCHDOG_START:
			if (wiFiOk) {
				watchdogStatus = WATCHDOG_VERIFY;
			}
			return;

		case WATCHDOG_VERIFY:
			if (wl_status != WL_CONNECTED) {
				watchdogWiFiLostCounter++;
			} else {
				watchdogWiFiLostCounter = 0;
			}

			if (watchdogWiFiLostCounter > WATCHDOG_MAX_NOT_CONNECTED) {
				watchdogWiFiLostCounter = 0;
				watchdogStatus = WATCHDOG_WIFI_OFF;
			}
			return;

		case WATCHDOG_WIFI_OFF:
			Serial.println("Watchdog reconnect.");
			WiFi.reconnect();
			watchdogStatus = WATCHDOG_WIFI_WAIT;
			return;

		case WATCHDOG_WIFI_WAIT:
			watchdogWiFiWaitCounter++;

			if (watchdogWiFiWaitCounter > WATCHDOG_MAX_WAIT) {
				watchdogWiFiWaitCounter = 0;
				watchdogStatus = WATCHDOG_VERIFY;
				Serial.println("Watchdog failed to reconnect.");
			}

			if (wl_status == WL_CONNECTED) {
				watchdogWiFiWaitCounter = 0;
				watchdogStatus = WATCHDOG_VERIFY;
				Serial.println("Watchdog successfully reconnected.");
			}
			return;
	}
}
