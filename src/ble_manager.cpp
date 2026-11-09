#include <Arduino.h>
#include <NimBLEDevice.h>

#include <string>

#include "ble_manager.h"
#include "audio_manager.h"
#include "ota_manager.h"

// =====================================================
// UUID CONFIGURATION
// =====================================================

#define DEVICE_NAME "Bose Amplifier"

#define AUDIO_SERVICE_UUID \
    "0000A001-0000-1000-8000-00805F9B34FB"

#define VOLUME_COMMAND_UUID \
    "0000A002-0000-1000-8000-00805F9B34FB"

#define VOLUME_STATE_UUID \
    "0000A003-0000-1000-8000-00805F9B34FB"

#define COMMAND_RESULT_UUID \
    "0000A004-0000-1000-8000-00805F9B34FB"

#define OTA_CONTROL_UUID \
    "0000A005-0000-1000-8000-00805F9B34FB"

#define OTA_STATUS_UUID \
    "0000A006-0000-1000-8000-00805F9B34FB"

// =====================================================
// PROTOCOL CONFIGURATION
// =====================================================

#define CMD_SET_VOLUME          0x01

#define RESULT_ACCEPTED         0x00
#define RESULT_INVALID_LENGTH   0x01
#define RESULT_UNSUPPORTED      0x02
#define RESULT_INVALID_CHANNEL  0x03
#define RESULT_INVALID_VOLUME   0x04

#define UNKNOWN_VALUE           0xFF

#define CMD_START_OTA 0x10

#define OTA_STATUS_IDLE     0x00
#define OTA_STATUS_ACCEPTED 0x01
#define OTA_STATUS_REJECTED 0x02

// =====================================================
// BLE OBJECTS
// =====================================================

static NimBLECharacteristic* volumeCommand = nullptr;
static NimBLECharacteristic* volumeState = nullptr;
static NimBLECharacteristic* commandResult = nullptr;
static NimBLECharacteristic* otaControl = nullptr;
static NimBLECharacteristic* otaStatus = nullptr;

static AudioManager* audioManager = nullptr;

// =====================================================
// NOTIFICATION FUNCTIONS
// =====================================================

static void notifyVolumeState()
{
    if (volumeState == nullptr || audioManager == nullptr)
    {
        return;
    }

    uint8_t volumes[7];

    audioManager->copyVolumes(volumes);

    volumeState->setValue(volumes, sizeof(volumes));

    bool success = volumeState->notify();

    if (success)
    {
        Serial.println(
            "[BLE] Seven-channel volume state notification sent"
        );
    }
    else
    {
        Serial.println(
            "[BLE] WARNING: Volume state notification "
            "not delivered (check client subscription)"
        );
    }
}

static void sendCommandResult(
    uint8_t commandId,
    uint8_t transactionId,
    uint8_t status,
    uint8_t channel,
    uint8_t volume
)
{
    if (commandResult == nullptr)
    {
        return;
    }

    uint8_t response[5] =
    {
        commandId,
        transactionId,
        status,
        channel,
        volume
    };

    commandResult->setValue(response, sizeof(response));

    bool success = commandResult->notify();

    if (success)
    {
        Serial.printf(
            "[BLE] Command result notification sent "
            "(TX=%u, STATUS=0x%02X)\n",
            transactionId,
            status
        );
    }
    else
    {
        Serial.println(
            "[BLE] WARNING: Command result notification "
            "not delivered (check client subscription)"
        );
    }
}

// =====================================================
// COMMAND PROCESSING
// =====================================================

static void processVolumeCommand(const std::string& packet)
{
    Serial.println();
    Serial.println("[BLE] Command received");

    // -------------------------------------------------
    // Validate packet length
    // -------------------------------------------------

    if (packet.size() != 4)
    {
        Serial.printf(
            "[BLE] ERROR: Invalid packet length: %u\n",
            static_cast<unsigned int>(packet.size())
        );

        sendCommandResult(
            UNKNOWN_VALUE,
            UNKNOWN_VALUE,
            RESULT_INVALID_LENGTH,
            UNKNOWN_VALUE,
            UNKNOWN_VALUE
        );

        return;
    }

    const uint8_t* data =
        reinterpret_cast<const uint8_t*>(packet.data());

    uint8_t commandId = data[0];
    uint8_t channel = data[1];
    uint8_t volume = data[2];
    uint8_t transactionId = data[3];

    Serial.printf("[BLE] Command ID : 0x%02X\n", commandId);
    Serial.printf("[BLE] Channel    : %u\n", channel);
    Serial.printf("[BLE] Volume     : %u\n", volume);
    Serial.printf("[BLE] Tx ID      : %u\n", transactionId);

    // -------------------------------------------------
    // Validate command ID
    // -------------------------------------------------

    if (commandId != CMD_SET_VOLUME)
    {
        Serial.printf(
            "[BLE] ERROR: Unsupported command 0x%02X\n",
            commandId
        );

        sendCommandResult(
            commandId,
            transactionId,
            RESULT_UNSUPPORTED,
            channel,
            volume
        );

        return;
    }

    // -------------------------------------------------
    // Update audio state
    // -------------------------------------------------

    bool changed = false;

    VolumeUpdateResult result =
        audioManager->setVolume(
            channel,
            volume,
            changed
        );

    uint8_t status = static_cast<uint8_t>(result);

    if (result != VolumeUpdateResult::ACCEPTED)
    {
        Serial.println("[BLE] Command rejected");

        sendCommandResult(
            commandId,
            transactionId,
            status,
            channel,
            volume
        );

        return;
    }

    Serial.println("[BLE] Command accepted");

    // -------------------------------------------------
    // Notify updated state only if it changed
    // -------------------------------------------------

    if (changed)
    {
        notifyVolumeState();
    }

    // -------------------------------------------------
    // Send application-level acknowledgement
    // -------------------------------------------------

    sendCommandResult(
        commandId,
        transactionId,
        RESULT_ACCEPTED,
        channel,
        volume
    );
}

// =====================================================
// BLE CHARACTERISTIC CALLBACK
// =====================================================

class VolumeCommandCallbacks :
    public NimBLECharacteristicCallbacks
{
    void onWrite(
        NimBLECharacteristic* characteristic,
        NimBLEConnInfo& connInfo
    ) override
    {
        (void)connInfo;

        std::string packet = characteristic->getValue();

        processVolumeCommand(packet);
    }
};

class OTAControlCallbacks :
    public NimBLECharacteristicCallbacks
{
    void onWrite(
        NimBLECharacteristic* characteristic,
        NimBLEConnInfo& connInfo
    ) override
    {
        (void)connInfo;

        std::string packet = characteristic->getValue();

        if (packet.size() != 1 ||
            static_cast<uint8_t>(packet[0]) != CMD_START_OTA)
        {
            uint8_t status = 0x04;
            otaStatus->setValue(&status, 1);
            otaStatus->notify();
            return;
        }

        Serial.println("[BLE] OTA start requested");

        requestOTAStart();

        uint8_t status = 0x01;
        otaStatus->setValue(&status, 1);
        otaStatus->notify();
    }
};

static OTAControlCallbacks otaCallbacks;

// =====================================================
// BLE SERVER CALLBACK
// =====================================================

class AudioServerCallbacks :
    public NimBLEServerCallbacks
{
    void onConnect(
        NimBLEServer* server,
        NimBLEConnInfo& connInfo
    ) override
    {
        (void)server;
        (void)connInfo;

        Serial.println();
        Serial.println("[BLE] Phone connected");

        Serial.println(
            "[BLE] Client should read Volume State "
            "for initial synchronization"
        );
    }

    void onDisconnect(
        NimBLEServer* server,
        NimBLEConnInfo& connInfo,
        int reason
    ) override
    {
        (void)connInfo;

        Serial.printf(
            "[BLE] Phone disconnected. Reason: %d\n",
            reason
        );

        Serial.println("[BLE] Restarting advertising");

        NimBLEDevice::startAdvertising();
    }
};

// Static callback objects
static VolumeCommandCallbacks volumeCallbacks;
static AudioServerCallbacks serverCallbacks;

// =====================================================
// BLE INITIALIZATION
// =====================================================

void initializeBLE(AudioManager& manager)
{
    audioManager = &manager;

    Serial.println("[BLE] Initializing BLE...");
    NimBLEDevice::init(DEVICE_NAME);

    NimBLEServer* server =
        NimBLEDevice::createServer();

    server->setCallbacks(&serverCallbacks);

    NimBLEService* audioService =
        server->createService(AUDIO_SERVICE_UUID);

    // Volume Command
    volumeCommand =
        audioService->createCharacteristic(
            VOLUME_COMMAND_UUID,
            NIMBLE_PROPERTY::WRITE
        );

    volumeCommand->setCallbacks(&volumeCallbacks);

    // Volume State
    volumeState =
        audioService->createCharacteristic(
            VOLUME_STATE_UUID,
            NIMBLE_PROPERTY::READ |
            NIMBLE_PROPERTY::NOTIFY
        );

    // Command Result
    commandResult =
        audioService->createCharacteristic(
            COMMAND_RESULT_UUID,
            NIMBLE_PROPERTY::NOTIFY
        );

    uint8_t initialVolumes[7];

    audioManager->copyVolumes(initialVolumes);

    volumeState->setValue(
        initialVolumes,
        sizeof(initialVolumes)
);

    // OTA Control
    otaControl =
        audioService->createCharacteristic(
            OTA_CONTROL_UUID,
            NIMBLE_PROPERTY::WRITE
        );

    otaControl->setCallbacks(&otaCallbacks);

    // OTA Status
    otaStatus =
        audioService->createCharacteristic(
            OTA_STATUS_UUID,
            NIMBLE_PROPERTY::READ |
            NIMBLE_PROPERTY::NOTIFY
        );

    uint8_t initialOTAStatus = OTA_STATUS_IDLE;

    otaStatus->setValue(
        &initialOTAStatus,
        sizeof(initialOTAStatus)
    );

    audioService->start();

    NimBLEAdvertising* advertising =
    NimBLEDevice::getAdvertising();

    advertising->setName(DEVICE_NAME);

    NimBLEAdvertisementData scanResponse;

    scanResponse.addServiceUUID(AUDIO_SERVICE_UUID);

    advertising->setScanResponseData(scanResponse);

    advertising->start();

    Serial.println("[BLE] BLE initialized successfully");
    Serial.println("[BLE] Device: SixChannelAudio");
    Serial.println("[BLE] Advertising started");
}
