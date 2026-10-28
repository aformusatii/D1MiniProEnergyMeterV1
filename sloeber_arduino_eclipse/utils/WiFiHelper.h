
#ifndef _WiFiHelper_H_
#define _WiFiHelper_H_

#include <WiFiUDP.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiScan.h>

#define WIFI_CHECK_INTERVAL_MS      500

#define STATUS_LED_NOT_CONNECTED    800
#define STATUS_LED_CON_LOST         200
#define STATUS_LED_CONNECTED       3000

#define WATCHDOG_START         0
#define WATCHDOG_VERIFY        1
#define WATCHDOG_WIFI_OFF      2
#define WATCHDOG_WIFI_WAIT     3


#define WATCHDOG_MAX_NOT_CONNECTED  100
#define WATCHDOG_MAX_WAIT           200

class WiFiHelper {

	private:
		std::function<void(void)> setup_callback;
		unsigned long wifi_check_timer;
		wl_status_t wifi_last_status;

		unsigned long status_led_timer;
		unsigned long ledInterval;
		uint8_t statusLEDPort;
		bool useStatusLED;
		uint8_t lastStatusLEDState;
		uint8_t watchdogStatus;
		uint8_t watchdogWiFiLostCounter;
		uint8_t watchdogWiFiWaitCounter;

		void blinkLedStatus(unsigned long);
		void calculateLedStatusFrequency(wl_status_t);
		void watchdogLoop(wl_status_t);

	public:
		WiFiHelper(void);

		void begin(const char *ssid,const char *key, std::function<void(void)> scallback);
		void begin(const char *ssid,const char *key, const uint8_t*, std::function<void(void)> scallback);
		bool configureStaticIp(uint8_t, uint8_t, uint8_t, uint8_t);
		void setupLedStatus(uint8_t);
		void loop(unsigned long);

		bool wiFiOk;

};

#endif /* _WiFiHelper_H_ */
