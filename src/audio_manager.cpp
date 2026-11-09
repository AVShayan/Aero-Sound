#include "audio_manager.h"

#include <cstring>

// =====================================================
// INITIALIZATION
// =====================================================

void AudioManager::begin()
{
    Serial.println("[AUDIO] Initializing audio manager...");

    // Initialize RAM defaults and ramp state.
    for (uint8_t i = 0; i < CHANNEL_COUNT; i++)
    {
        channelVolumes[i] = DEFAULT_VOLUME;
        targetPercent[i] = DEFAULT_VOLUME;
        currentPercent[i] = -1;
        lastRampMs[i] = 0;
    }

    // -------------------------------------------------
    // Restore volume configuration from NVS
    // -------------------------------------------------

    bool nvsReady = preferences.begin("audio", false);

    if (!nvsReady)
    {
        Serial.println(
            "[NVS] ERROR: Failed to open namespace. "
            "Using RAM defaults."
        );
    }
    else
    {
        Serial.println("[NVS] Namespace opened: audio");

        uint8_t storedVolumes[CHANNEL_COUNT] = {0};
        bool validData = false;
        bool migrationRequired = false;

        size_t storedLength =
            preferences.getBytesLength("volumes");

        // Existing six-channel configuration.
        if (storedLength == 6)
        {
            size_t bytesRead = preferences.getBytes(
                "volumes",
                storedVolumes,
                6
            );

            if (bytesRead == 6)
            {
                validData = true;

                for (uint8_t i = 0; i < 6; i++)
                {
                    if (storedVolumes[i] > 100)
                    {
                        validData = false;
                        break;
                    }
                }

                if (validData)
                {
                    storedVolumes[6] = DEFAULT_VOLUME;
                    migrationRequired = true;
                }
            }
        }
        // New seven-channel configuration.
        else if (storedLength == CHANNEL_COUNT)
        {
            size_t bytesRead = preferences.getBytes(
                "volumes",
                storedVolumes,
                CHANNEL_COUNT
            );

            if (bytesRead == CHANNEL_COUNT)
            {
                validData = true;

                for (uint8_t i = 0; i < CHANNEL_COUNT; i++)
                {
                    if (storedVolumes[i] > 100)
                    {
                        validData = false;
                        break;
                    }
                }
            }
        }

        if (validData)
        {
            memcpy(
                channelVolumes,
                storedVolumes,
                CHANNEL_COUNT
            );

            if (migrationRequired)
            {
                Serial.println(
                    "[NVS] Migrating six-channel configuration "
                    "to seven channels..."
                );

                if (saveToNVS(channelVolumes))
                {
                    Serial.println(
                        "[NVS] Migration successful. "
                        "Channel 7 initialized to 50%."
                    );
                }
                else
                {
                    Serial.println(
                        "[NVS] ERROR: Migration save failed."
                    );
                }
            }
            else
            {
                Serial.println(
                    "[NVS] Seven-channel configuration restored"
                );
            }
        }
        else
        {
            Serial.println(
                "[NVS] No valid configuration found"
            );

            Serial.println(
                "[NVS] Initializing defaults: 50%"
            );

            for (uint8_t i = 0; i < CHANNEL_COUNT; i++)
            {
                channelVolumes[i] = DEFAULT_VOLUME;
            }

            if (saveToNVS(channelVolumes))
            {
                Serial.println(
                    "[NVS] Default configuration saved"
                );
            }
            else
            {
                Serial.println(
                    "[NVS] ERROR: Failed to save defaults"
                );
            }
        }
    }

    // Synchronize hardware targets with loaded values.
    for (uint8_t i = 0; i < CHANNEL_COUNT; i++)
    {
        targetPercent[i] = channelVolumes[i];
    }

    // -------------------------------------------------
    // Initialize I2C and both PT2258 ICs
    // -------------------------------------------------

    Serial.println("[PT2258] Initializing I2C...");

    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(20000);

    // Primary PT2258: address 0x40.
    Wire.beginTransmission(PT2258_PRIMARY_ADDR);
    Wire.write(0xC0);

    pt2258PrimaryReady =
        (Wire.endTransmission() == 0);

    if (pt2258PrimaryReady)
    {
        Serial.println(
            "[PT2258] Primary IC initialized at 0x40"
        );
    }
    else
    {
        Serial.println(
            "[PT2258] ERROR: Primary IC initialization failed"
        );
    }

    // Secondary PT2258: address 0x42.
    Wire.beginTransmission(PT2258_SECONDARY_ADDR);
    Wire.write(0xC0);

    pt2258SecondaryReady =
        (Wire.endTransmission() == 0);

    if (pt2258SecondaryReady)
    {
        Serial.println(
            "[PT2258] Secondary IC initialized at 0x42"
        );
    }
    else
    {
        Serial.println(
            "[PT2258] ERROR: Secondary IC initialization failed"
        );
    }

    // -------------------------------------------------
    // Print loaded configuration
    // -------------------------------------------------

    Serial.println(
        "[AUDIO] Current seven-channel configuration:"
    );

    for (uint8_t i = 0; i < CHANNEL_COUNT; i++)
    {
        Serial.printf(
            "[AUDIO] Channel %u: %u%%\n",
            i + 1,
            channelVolumes[i]
        );
    }

    Serial.println("[AUDIO] Initialization complete");
}


// =====================================================
// VERIFIED MULTI-SEGMENT AUDIO TAPER CURVE
// =====================================================

int AudioManager::percentToAttenuationLog(int percent)
{
    percent = constrain(percent, 0, 100);

    if (percent == 0)
    {
        return 79; // Dead silent
    }

    int attenuation = 79;

    // Segment 1: Low Volume Range (1% to 15%)
    if (percent <= 15)
    {
        attenuation = map(percent, 1, 15, 79, 46);
    }
    // Segment 2: Main Listening Range (16% to 70%)
    else if (percent <= 70)
    {
        attenuation = map(percent, 16, 70, 45, 18);
    }
    // Segment 3: High Power Range (71% to 100%)
    else
    {
        attenuation = map(
            percent,
            71,
            100,
            17,
            MAX_OUTPUT_ATTENUATION_DB
        );
    }

    return constrain(
        attenuation,
        MAX_OUTPUT_ATTENUATION_DB,
        79
    );
}


// =====================================================
// LOW-LEVEL PT2258 I2C TRANSMISSION
// =====================================================

bool AudioManager::setChannelAttenuation(
    uint8_t address,
    uint8_t channel,
    int attenuationDb
)
{
    if (channel < 1 || channel > 6)
    {
        return false;
    }

    attenuationDb = constrain(attenuationDb, 0, 79);

    uint8_t tens = attenuationDb / 10;
    uint8_t ones = attenuationDb % 10;

    // PT2258 channel command bases.
    static const uint8_t tenDbBase[6] =
    {
        0x80, // Channel 1
        0x40, // Channel 2
        0x00, // Channel 3
        0x20, // Channel 4
        0x60, // Channel 5
        0xA0  // Channel 6
    };

    static const uint8_t oneDbBase[6] =
    {
        0x90, // Channel 1
        0x50, // Channel 2
        0x10, // Channel 3
        0x30, // Channel 4
        0x70, // Channel 5
        0xB0  // Channel 6
    };

    uint8_t index = channel - 1;

    uint8_t tenDbCommand =
        tenDbBase[index] | tens;

    uint8_t oneDbCommand =
        oneDbBase[index] | ones;

    Wire.beginTransmission(address);

    Wire.write(tenDbCommand);
    Wire.write(oneDbCommand);

    return Wire.endTransmission() == 0;
}


// =====================================================
// SET VOLUME
// =====================================================

VolumeUpdateResult AudioManager::setVolume(
    uint8_t channel,
    uint8_t volume,
    bool &changed
)
{
    changed = false;

    if (channel >= CHANNEL_COUNT)
    {
        Serial.printf(
            "[AUDIO] ERROR: Invalid channel index %u\n",
            channel
        );

        return VolumeUpdateResult::INVALID_CHANNEL;
    }

    if (volume > 100)
    {
        Serial.printf(
            "[AUDIO] ERROR: Invalid volume %u\n",
            volume
        );

        return VolumeUpdateResult::INVALID_VOLUME;
    }

    uint8_t previousVolume;

    portENTER_CRITICAL(&volumeMux);

    previousVolume = channelVolumes[channel];

    if (previousVolume != volume)
    {
        channelVolumes[channel] = volume;

        // All seven logical channels now control hardware.
        targetPercent[channel] = volume;

        persistencePending = true;
        lastChangeMs = millis();

        changed = true;
    }

    portEXIT_CRITICAL(&volumeMux);

    if (changed)
    {
        Serial.printf(
            "[AUDIO] Channel %u updated: %u%% -> %u%%\n",
            channel + 1,
            previousVolume,
            volume
        );
    }
    else
    {
        Serial.printf(
            "[AUDIO] Channel %u already at %u%%\n",
            channel + 1,
            volume
        );
    }

    return VolumeUpdateResult::ACCEPTED;
}


// =====================================================
// HARDWARE VOLUME RAMP PROCESSING
// =====================================================

void AudioManager::processHardware()
{
    uint8_t targets[CHANNEL_COUNT];

    // Copy target values safely.
    portENTER_CRITICAL(&volumeMux);

    memcpy(
        targets,
        targetPercent,
        CHANNEL_COUNT
    );

    portEXIT_CRITICAL(&volumeMux);

    uint32_t now = millis();

    // Process all seven hardware channels.
    for (uint8_t index = 0; index < CHANNEL_COUNT; index++)
    {
        uint8_t address;
        uint8_t ptChannel;

        // -------------------------------------------------
        // Hardware mapping
        // -------------------------------------------------

        if (index < 6)
        {
            address = PT2258_PRIMARY_ADDR;
            ptChannel = index + 1;
        }
        else
        {
            address = PT2258_SECONDARY_ADDR;
            ptChannel = 1;
        }

        bool chipReady =
            (address == PT2258_PRIMARY_ADDR)
                ? pt2258PrimaryReady
                : pt2258SecondaryReady;

        if (!chipReady)
        {
            continue;
        }

        uint8_t target = targets[index];

        // -------------------------------------------------
        // First hardware application after startup
        // -------------------------------------------------

        if (currentPercent[index] < 0)
        {
            int attenuation =
                percentToAttenuationLog(target);

            if (setChannelAttenuation(
                    address,
                    ptChannel,
                    attenuation))
            {
                currentPercent[index] = target;

                Serial.printf(
                    "[PT2258] Address 0x%02X CH%u initialized: "
                    "%u%%, attenuation %d dB\n",
                    address,
                    ptChannel,
                    target,
                    attenuation
                );
            }
            else
            {
                Serial.printf(
                    "[PT2258] ERROR: Address 0x%02X "
                    "CH%u initialization failed\n",
                    address,
                    ptChannel
                );
            }

            continue;
        }

        // -------------------------------------------------
        // Check if a volume ramp is required
        // -------------------------------------------------

        if (currentPercent[index] == target)
        {
            continue;
        }

        if ((uint32_t)(now - lastRampMs[index]) <
            RAMP_INTERVAL_MS)
        {
            continue;
        }

        lastRampMs[index] = now;

        int previousPercent = currentPercent[index];
        int nextPercent = previousPercent;

        if (target > previousPercent)
        {
            nextPercent++;
        }
        else
        {
            nextPercent--;
        }

        int attenuation =
            percentToAttenuationLog(nextPercent);

        // -------------------------------------------------
        // Apply next ramp step
        // -------------------------------------------------

        if (setChannelAttenuation(
                address,
                ptChannel,
                attenuation))
        {
            currentPercent[index] = nextPercent;

            Serial.printf(
                "[PT2258] Address 0x%02X CH%u: "
                "%d%% -> %d%% (%d dB)\n",
                address,
                ptChannel,
                previousPercent,
                nextPercent,
                attenuation
            );
        }
        else
        {
            Serial.printf(
                "[PT2258] ERROR: Address 0x%02X "
                "CH%u I2C write failed\n",
                address,
                ptChannel
            );
        }
    }
}


// =====================================================
// COPY VOLUME STATE
// =====================================================

void AudioManager::copyVolumes(uint8_t destination[7])
{
    portENTER_CRITICAL(&volumeMux);

    memcpy(
        destination,
        channelVolumes,
        CHANNEL_COUNT
    );

    portEXIT_CRITICAL(&volumeMux);
}


// =====================================================
// SAVE TO NVS
// =====================================================

bool AudioManager::saveToNVS(
    const uint8_t values[CHANNEL_COUNT]
)
{
    size_t bytesWritten = preferences.putBytes(
        "volumes",
        values,
        CHANNEL_COUNT
    );

    return bytesWritten == CHANNEL_COUNT;
}


// =====================================================
// PERSISTENCE PROCESSING
// =====================================================

void AudioManager::processPersistence()
{
    uint32_t now = millis();

    uint8_t snapshot[CHANNEL_COUNT];

    portENTER_CRITICAL(&volumeMux);

    if (!persistencePending)
    {
        portEXIT_CRITICAL(&volumeMux);
        return;
    }

    // Wait 500 ms after the most recent change.
    if ((uint32_t)(now - lastChangeMs) < SAVE_DELAY_MS)
    {
        portEXIT_CRITICAL(&volumeMux);
        return;
    }

    // Retry failed writes after 5 seconds.
    if (lastSaveAttemptMs != 0 &&
        (uint32_t)(now - lastSaveAttemptMs) < RETRY_DELAY_MS)
    {
        portEXIT_CRITICAL(&volumeMux);
        return;
    }

    memcpy(
        snapshot,
        channelVolumes,
        CHANNEL_COUNT
    );

    lastSaveAttemptMs = now;

    portEXIT_CRITICAL(&volumeMux);

    Serial.println("[NVS] Saving volume configuration...");

    bool success = saveToNVS(snapshot);

    portENTER_CRITICAL(&volumeMux);

    if (success &&
        memcmp(
            snapshot,
            channelVolumes,
            CHANNEL_COUNT
        ) == 0)
    {
        persistencePending = false;
    }

    portEXIT_CRITICAL(&volumeMux);

    if (success)
    {
        Serial.println(
            "[NVS] Seven-channel volume configuration "
            "saved successfully"
        );
    }
    else
    {
        Serial.println(
            "[NVS] ERROR: Save failed. Retry scheduled."
        );
    }
}