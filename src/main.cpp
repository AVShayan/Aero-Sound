#include <Arduino.h>

#include "audio_manager.h"
#include "ble_manager.h"
#include "ota_manager.h"

AudioManager audioManager;

void setup(){
    pinMode(2, OUTPUT);
    digitalWrite(2, HIGH);
    Serial.begin(115200);
    delay(1000);

    audioManager.begin();

    initializeBLE(audioManager);

    initializeOTA();
}

void loop(){
    audioManager.processHardware();

    audioManager.processPersistence();

    processOTA();

    delay(1);
}