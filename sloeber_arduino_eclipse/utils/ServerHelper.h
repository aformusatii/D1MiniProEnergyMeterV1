
#ifndef _ServerHelper_H_
#define _ServerHelper_H_

#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>

class ServerHelper {

	private:
		char* title;
		ESP8266WebServer* server;

	public:
		ServerHelper(char *, ESP8266WebServer*);

		void indexPage();
		void handleNotFound();

};

#endif /* _ServerHelper_H_ */
