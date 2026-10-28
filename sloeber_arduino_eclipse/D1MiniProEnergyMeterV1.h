// Only modify this file to include
// - function definitions (prototypes)
// - include files
// - extern variable definitions
// In the appropriate section

#ifndef _D1MiniProEnergyMeterV1_H_
#define _D1MiniProEnergyMeterV1_H_
#include "Arduino.h"
//add your includes for the project D1MiniProEnergyMeterV1 here
#include "WiFiHelper.h"
#include "ServerHelper.h"
#include "Logger.h"

#include <WiFiConfig.h>

#include <PZEM004Tv30.h>

//end of add your includes here


//add your function definitions for the project D1MiniProEnergyMeterV1 here
void setupAfterWiFiConnected();

void indexPage();
void handleNotFound();
void health();
void sendSensorData();
void writeJson(int httpStatus, const JsonDocument &doc);
void setupHTTPActions();

void loopPZem();

//Do not add code below this line
#endif /* _D1MiniProEnergyMeterV1_H_ */
