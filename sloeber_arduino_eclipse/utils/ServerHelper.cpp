#include "ServerHelper.h"

ServerHelper::ServerHelper(char *_title, ESP8266WebServer *_server) {
	title = _title;
	server = _server;
}

void ServerHelper::indexPage() {
	  String message = "<!doctype html>";
	  message += "<html lang=\"en\">";
	  message += "<head>";
	  message += "<title>";
	  message.concat(title);
	  message += "</title>";
	  message += "</head>";
	  message += "<body>";

	  message += "<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Update'></form>";

	  message += "--------------------------------------------------------------<br/>";

	  uint32_t realSize = ESP.getFlashChipRealSize();
	  uint32_t ideSize = ESP.getFlashChipSize();
	  uint32_t chipId = ESP.getFlashChipId();
	  uint32_t chipSpeed = ESP.getFlashChipSpeed();
	  FlashMode_t ideMode = ESP.getFlashChipMode();

	  char buf[255];

	  sprintf(buf, "Flash chip id: %08X<br/>Flash chip speed: %u<br/>Flash chip size: %u<br/>Flash ide mode: %s<br/>Flash real size: %u<br/>",
	    chipId,
	    chipSpeed,
	    ideSize,
	    (ideMode == FM_QIO ? "QIO" : ideMode == FM_QOUT ? "QOUT" : ideMode == FM_DIO ? "DIO" : ideMode == FM_DOUT ? "DOUT" : "UNKNOWN"),
	    realSize);

	  message += buf;

	  message += "--------------------------------------------------------------<br/>";

	  message += "<br/>SSID: ";
	  message.concat(WiFi.SSID());
	  message.concat("<br/>MAC: ");
	  message.concat(WiFi.BSSIDstr());
	  /*
	    Signal Strength TL;DR   Required for
	    -30 dBm Amazing Max achievable signal strength. The client can only be a few feet from the AP to achieve this. Not typical or desirable in the real world.  N/A
	    -67 dBm Very Good Minimum signal strength for applications that require very reliable, timely delivery of data packets. VoIP/VoWiFi, streaming video
	    -70 dBm Okay  Minimum signal strength for reliable packet delivery. Email, web
	    -80 dBm Not Good  Minimum signal strength for basic connectivity. Packet delivery may be unreliable.  N/A
	    -90 dBm Unusable  Approaching or drowning in the noise floor. Any functionality is highly unlikely. N/A
	  */
	  message += "<br/>RSSI: ";
	  message.concat(WiFi.RSSI());
	  message += "<br/>CH: ";
	  message.concat(WiFi.channel());

	  message += "</body>";

	  message += "</html>";

	  server->send(200, "text/html", message);
}

void ServerHelper::handleNotFound() {
	  String message = "File Not Found\n\n";
	  message += "URI: ";
	  message += server->uri();
	  message += "\nMethod: ";
	  message += (server->method() == HTTP_GET) ? "GET" : "POST";
	  message += "\nArguments: ";
	  message += server->args();
	  message += "\n";

	  for (uint8_t i=0; i<server->args(); i++){
	    message += " " + server->argName(i) + ": " + server->arg(i) + "\n";
	  }

	  server->send(404, "text/plain", message);
}
