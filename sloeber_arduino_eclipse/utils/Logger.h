
#ifndef _Logger_H_
#define _Logger_H_

#include <WiFiUDP.h>
#include <Arduino.h>
#include <ArduinoJson.h>

class Logger {

	private:
		void send(JsonDocument &doc, const char *level, const char *msg);

	public:
		void info(const char *msg);
		void info(JsonDocument &doc, const char *msg);

		void error(const char *msg);
		void error(JsonDocument &doc, const char *msg);

		void info2(const char *msg);

};

#endif /* _Logger_H_ */
