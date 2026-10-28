#ifndef _ServerHelper_H_
#define _ServerHelper_H_

#include <Arduino.h>
#include <ESP8266WebServer.h>

class ServerHelper {

	private:
		const char* title;
		ESP8266WebServer* server;

	public:
		ServerHelper(const char *, ESP8266WebServer*);

		void indexPage();
		void handleNotFound();

};

#endif /* _ServerHelper_H_ */
