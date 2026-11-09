#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>

#include "ota_manager.h"

// =====================================================
// WIFI ACCESS POINT CONFIGURATION
// =====================================================

static const char* OTA_SSID = "Bose Amplifier";
static const char* OTA_PASSWORD = "Audi@7007";

// =====================================================
// OTA SERVER
// =====================================================

static WebServer server(80);

static volatile bool startRequested = false;
static bool otaActive = false;
static bool uploadFailed = false;
static bool uploadFinished = false;
static bool restartRequested = false;
static OTAStatus otaStatus = OTAStatus::IDLE;

// =====================================================
// OTA WEBPAGE
// =====================================================

static const char OTA_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Audio Controller OTA</title>
</head>
<body>
    <h2>Audio Controller Firmware Update</h2>

    <form id="uploadForm">
        <input type="file" name="firmware" accept=".bin" required>
        <br><br>
        <button type="submit">Upload Firmware</button>
    </form>

    <p id="status">Ready</p>

    <script>
        document.getElementById("uploadForm").onsubmit = async function(e) {
            e.preventDefault();

            const status = document.getElementById("status");
            const formData = new FormData(this);

            status.innerText = "Uploading... Do not disconnect power.";

            try {
                const response = await fetch("/update", {
                    method: "POST",
                    body: formData
                });

                status.innerText = await response.text();
            }
            catch (error) {
                status.innerText = "Connection interrupted. Check device status.";
            }
        };
    </script>
</body>
</html>
)rawliteral";

// =====================================================
// HTTP ROUTES
// =====================================================

static void handleUpload()
{
    if (uploadFinished && !uploadFailed && !Update.hasError())
    {
        server.send(200, "text/plain",
                    "Firmware uploaded successfully. Restarting...");
        restartRequested = true;
    }
    else
    {
        server.send(500, "text/plain",
                    "Firmware update failed. Existing firmware should remain active.");
        digitalWrite(2, HIGH);

    }
}

static void handleUploadData()
{
    HTTPUpload& upload = server.upload();

    if (upload.status == UPLOAD_FILE_START)
    {
        Serial.printf(
            "[OTA] Upload started: %s\n",
            upload.filename.c_str()
        );

        uploadFailed = false;
        uploadFinished = false;
        

        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH))
        {
            Update.printError(Serial);
            uploadFailed = true;
        }
    }
    else if (upload.status == UPLOAD_FILE_WRITE)
    {
        if (!uploadFailed)
        {
            size_t written = Update.write(
                upload.buf,
                upload.currentSize
            );

            if (written != upload.currentSize)
            {
                Serial.println("[OTA] ERROR: Firmware write failed");
                Update.printError(Serial);
                uploadFailed = true;
            }
        }
    }
    else if (upload.status == UPLOAD_FILE_END)
    {
        if (!uploadFailed && Update.end(true))
        {
            Serial.printf(
                "[OTA] Upload complete: %u bytes\n",
                upload.totalSize
            );

            uploadFinished = true;
            otaStatus = OTAStatus::SUCCESS;
        }
        else
        {
            Serial.println("[OTA] ERROR: Firmware finalization failed");
            Update.printError(Serial);
            uploadFailed = true;
            otaStatus = OTAStatus::FAILED;
        }
    }
    else if (upload.status == UPLOAD_FILE_ABORTED)
    {
        Serial.println("[OTA] Upload aborted");

        Update.abort();
        uploadFailed = true;
        otaStatus = OTAStatus::FAILED;
    }
}

// =====================================================
// START OTA ACCESS POINT
// =====================================================

static void startOTAServer()
{
    Serial.println();
    Serial.println("[OTA] Starting OTA mode...");

    WiFi.mode(WIFI_AP);
    WiFi.setSleep(false);

    if (!WiFi.softAP(OTA_SSID, OTA_PASSWORD))
{
    Serial.println("[OTA] ERROR: Access point creation failed");

    otaStatus = OTAStatus::FAILED;
    WiFi.mode(WIFI_OFF);
    return;
}

    server.on("/", HTTP_GET, []()
    {
        server.send_P(200, "text/html", OTA_PAGE);
    });

    server.on(
        "/update",
        HTTP_POST,
        handleUpload,
        handleUploadData
    );

    server.begin();
    otaStatus = OTAStatus::READY;

    otaActive = true;

    Serial.println("[OTA] Access point started");
    Serial.printf("[OTA] SSID: %s\n", OTA_SSID);
    Serial.printf("[OTA] IP: %s\n",
                  WiFi.softAPIP().toString().c_str());

    Serial.println("[OTA] Open http://192.168.4.1");
}

// =====================================================
// PUBLIC FUNCTIONS
// =====================================================

void initializeOTA()
{
    Serial.println("[OTA] OTA manager initialized");
}

void requestOTAStart()
{
    if (!otaActive && !startRequested)
    {
        startRequested = true;
        otaStatus = OTAStatus::STARTING;
    }
}

void processOTA()
{
    if (startRequested && !otaActive)
    {
        startRequested = false;
        startOTAServer();
    }

    if (otaActive)
    {
        server.handleClient();
    }

    if (restartRequested)
    {
        Serial.println("[OTA] Restarting into new firmware...");
        delay(1000);
        ESP.restart();
    }
}

bool isOTAActive()
{
    return otaActive;
}