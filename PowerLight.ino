#include <Arduino.h>

#include <ESP8266HTTPClient.h>
#include <FastLED.h>
#include <ArduinoJson.h>

#include <EEPROM.h>
#include <ArduinoOTA.h>

#include <PubSubClient.h>

#include "WiFiManager.h"
#include "FastLED.h"

#include "cmdproc.h"
#include "editline.h"

#define printf Serial.printf
#define POLL_INTERVAL   60000L
#define NUM_LEDS        60
#define DEFAULT_BRIGHTNESS 32

#define DATA_PIN_LED    D4

typedef struct {
    char ota_password[64];
} savedata_t;

static savedata_t savedata;

static WiFiManager wifiManager;
static WiFiManagerParameter otaPasswordParam("otapassword", "OTA password", "power", sizeof(savedata_t) - 1);
static WiFiClient wifiClient;
static CRGB ledring[NUM_LEDS];
static char espid[64] { 0 };
static char line[120] { 0 };
static PubSubClient mclient(wifiClient);
static bool showRing = false;
static bool prevState = false;
static bool updating = false;

static void mcallback(char *topic, byte *payload, unsigned int length)
{
    bool showRing = payload[0] == '1';

    if (showRing != prevState) {
        FastLED.setBrightness(showRing ? DEFAULT_BRIGHTNESS : 0);
        FastLED.show();

        prevState = showRing;
    }
}

static void wifiSaveConfigCallback(void)
{
    strcpy(savedata.ota_password, otaPasswordParam.getValue());
    EEPROM.put(0, savedata);
    EEPROM.commit();
}

static bool fetch_url(const char *host, int port, const char *path, String & response)
{
    HTTPClient httpClient;
    httpClient.begin(wifiClient, host, port, path, false);
    httpClient.setTimeout(20000);
    httpClient.setUserAgent(espid);

    printf("> GET http://%s:%d%s\n", host, port, path);
    int res = httpClient.GET();

    // evaluate result
    bool result = (res == HTTP_CODE_OK);
    response = result ? httpClient.getString() : httpClient.errorToString(res);
    httpClient.end();
    printf("< %d: %s\n", res, response.c_str());
    return result;
}

static void process_message(DynamicJsonDocument & doc)
{
    long energy = 0;
    int next = 0;
    int index = 0;
    int total = doc["total"];
    for (JsonObject item:doc["mix"].as < JsonArray > ()) {
        const char *item_id = item["id"];       // "solar", "wind", "fossil", "nuclear", "other", "waste"
        int item_power = item["power"]; // 3924, 121, 3951, 465, 40, 69
        const char *item_color = item["color"]; // "#FFFF00", "#0000FF", "#FF0000", "#FF00FF", ...

        energy += item_power;
        next = (NUM_LEDS * energy) / total;
        printf("led %2d-%2d: %s (%s)\n", index, next, item_color, item_id);
        for (int i = index; i < next; i++) {
            uint32_t rgb = strtoul(item_color + 1, NULL, 16);
            CRGB color = CRGB(rgb);
            if (i < NUM_LEDS) {
                ledring[i] = color;
            }
        }

        index = next;
    }

    FastLED.show();
}

static bool fetch_energy(void)
{
    DynamicJsonDocument doc(1024);
    String response;

    // fetch
    if (fetch_url("stofradar.nl", 9001, "/energy/latest", response)) {
        // decode
        if (deserializeJson(doc, response) == DeserializationError::Ok) {
            process_message(doc);
            return true;
        }
    }
    return false;
}

static int do_reboot(int argc, char *argv[])
{
    ESP.restart();
    return CMD_OK;
}

static int do_led(int argc, char *argv[])
{
    if (argc < 2) {
        return -1;
    }
    int rgb = strtoul(argv[1], NULL, 16);
    CRGB color = CRGB(rgb);
    FastLED.showColor(color);

    return CMD_OK;
}

static int do_get(int argc, char *argv[])
{
    fetch_energy();
    return CMD_OK;
}

static void show_help(const cmd_t * cmds)
{
    for (const cmd_t * cmd = cmds; cmd->cmd != NULL; cmd++) {
        printf("%10s: %s\n", cmd->name, cmd->help);
    }
}

static int do_help(int argc, char *argv[]);
static const cmd_t commands[] = {
    { "help", do_help, "Show help" },
    { "get", do_get, "Get a new value from the REST service" },
    { "led", do_led, "<RRGGBB> Set the LED to a specific value (hex)" },
    { "reboot", do_reboot, "Reboot" },
    { NULL, NULL, NULL }
};

static int do_help(int argc, char *argv[])
{
    show_help(commands);
    return CMD_OK;
}

void setup(void)
{
    EEPROM.begin(sizeof(savedata));
    EEPROM.get(0, savedata);

    snprintf(espid, sizeof(espid), "esp8266-powerlight-%06x", ESP.getChipId());

    Serial.begin(115200);
    printf("\nPOWERLIGHT\n");
    EditInit(line, sizeof(line));

    // configure LED ring    
    FastLED.addLeds < WS2812B, DATA_PIN_LED, GRB > (ledring,
                                                    NUM_LEDS).setCorrection(TypicalSMD5050);
    FastLED.setBrightness(DEFAULT_BRIGHTNESS);
    FastLED.showColor(CRGB::DarkOrange);

    // connect to wifi
    printf("Starting WIFI manager (%s)...\n", WiFi.SSID().c_str());
    wifiManager.addParameter(&otaPasswordParam);
    wifiManager.setSaveConfigCallback(wifiSaveConfigCallback);
    wifiManager.setConfigPortalTimeout(120);
    wifiManager.autoConnect("ESP-POWERLIGHT");

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        if (!updating) {
            memset(ledring, 0x00, sizeof ledring);

            FastLED.show();

            updating = true;

            FastLED.setBrightness(DEFAULT_BRIGHTNESS);
        }

        for(unsigned i=0; i<progress * NUM_LEDS / total; i++)
            ledring[i] = CRGB::Pink;

        FastLED.show();
    });

    ArduinoOTA.begin();

    mclient.setServer("mqtt.vm.nurd.space", 1883);
    mclient.setCallback(mcallback);

    FastLED.showColor(CRGB::Black);
}

void loop(void)
{
    mclient.loop();

    if (!mclient.connected()) {
        Serial.println(F("Attempting MQTT connection... "));

        if (mclient.connect("powerlight")) {
            mclient.subscribe("space/statedigit");

            Serial.println(F("Connected to MQTT server"));
        }
    }

    // fetch a new value every POLL_INTERVAL
    static unsigned int period_last = -1;
    unsigned int period = millis() / POLL_INTERVAL;
    if (period != period_last && updating == false) {
        period_last = period;

        fetch_energy();
    }

    // parse command line
    while (Serial.available()) {
        char c = Serial.read();
        bool haveLine = EditLine(c, &c);
        Serial.write(c);
        if (haveLine) {
            int result = cmd_process(commands, line);
            switch (result) {
            case CMD_OK:
                printf("OK\n");
                break;
            case CMD_NO_CMD:
                break;
            case CMD_UNKNOWN:
                printf("Unknown command, available commands:\n");
                show_help(commands);
                break;
            default:
                printf("%d\n", result);
                break;
            }
            printf(">");
        }
    }

    ArduinoOTA.handle();
}
