
#pragma once

#include <stdint.h>

enum class OTAStatus : uint8_t
{
    IDLE = 0x00,
    STARTING = 0x01,
    READY = 0x02,
    SUCCESS = 0x03,
    FAILED = 0x04
};

void initializeOTA();
void requestOTAStart();
void processOTA();