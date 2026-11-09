#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <Preferences.h>
#include <Wire.h>

#include "freertos/FreeRTOS.h"

enum class VolumeUpdateResult : uint8_t
{
    ACCEPTED = 0x00,
    INVALID_CHANNEL = 0x03,
    INVALID_VOLUME = 0x04
};

class AudioManager
{
public:
    void begin();

    VolumeUpdateResult setVolume(
        uint8_t channel,
        uint8_t volume,
        bool &changed
    );

    void copyVolumes(uint8_t destination[7]);

    void processHardware();

    void processPersistence();

private:
    static constexpr uint8_t CHANNEL_COUNT = 7;
    static constexpr uint8_t DEFAULT_VOLUME = 50;

    static constexpr uint32_t SAVE_DELAY_MS = 500;
    static constexpr uint32_t RETRY_DELAY_MS = 5000;

    // PT2258 configuration
    static constexpr int SDA_PIN = 21;
    static constexpr int SCL_PIN = 22;

    static constexpr uint8_t PT2258_PRIMARY_ADDR = 0x40;
    static constexpr uint8_t PT2258_SECONDARY_ADDR = 0x42;

    static constexpr int MAX_OUTPUT_ATTENUATION_DB = 7;

    static constexpr uint32_t RAMP_INTERVAL_MS = 4;

    uint8_t channelVolumes[CHANNEL_COUNT];

    // Hardware ramp state
    uint8_t targetPercent[CHANNEL_COUNT];
    int16_t currentPercent[CHANNEL_COUNT];
    uint32_t lastRampMs[CHANNEL_COUNT];

    // Independent initialization status
    bool pt2258PrimaryReady = false;
    bool pt2258SecondaryReady = false;

    Preferences preferences;

    bool persistencePending = false;

    uint32_t lastChangeMs = 0;
    uint32_t lastSaveAttemptMs = 0;

    // Protect shared volume state
    portMUX_TYPE volumeMux = portMUX_INITIALIZER_UNLOCKED;

    int percentToAttenuationLog(int percent);

    bool setChannelAttenuation(
        uint8_t address,
        uint8_t channel,
        int attenuationDb
    );

    bool saveToNVS(const uint8_t values[CHANNEL_COUNT]);
};