#include <Arduino.h>

#include <ESP8266HTTPClient.h>
#include <FastLED.h>
#include <ArduinoJson.h>

#include "WiFiManager.h"
#include "FastLED.h"

#include "cmdproc.h"
#include "editline.h"

#define printf Serial.printf
#define POLL_INTERVAL   60000L

#define PIN_LED_OUT    D3       // to DIN on the LED strip
#define PIN_LED_IN     D2       // to DOUT on the LED strip

#define MAX_LEDS       60

static WiFiManager wifiManager;
static WiFiClient wifiClient;
static CRGB ledring[MAX_LEDS + 1];
static int num_pixels = MAX_LEDS;
static char espid[64];
static char line[120];

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
    int index = 0;
    int total = doc["total"];
    for (JsonObject item:doc["mix"].as < JsonArray > ()) {
        const char *item_id = item["id"];       // "solar", "wind", "fossil", "nuclear", "other", "waste"
        int item_power = item["power"]; // 3924, 121, 3951, 465, 40, 69
        const char *item_color = item["color"]; // "#FFFF00", "#0000FF", "#FF0000", "#FF00FF", ...

        energy += item_power;
        int next = (num_pixels * energy) / total;
        printf("led %2d-%2d: %s (%s)\n", index, next, item_color, item_id);
        for (int i = index; i < next; i++) {
            uint32_t rgb = strtoul(item_color + 1, NULL, 16);
            CRGB color = CRGB(rgb);
            if (i < num_pixels) {
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
    if (fetch_url("stofradar.nl", 9001, "/electricity/generation", response)) {
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

static volatile int int_count;

static void ICACHE_RAM_ATTR led_in_interrupt(void)
{
    int_count++;
}

static int ring_probe(int pin_out, struct CRGB *data, int n)
{
    int_count = 0;
    FastLED.addLeds < WS2812B, PIN_LED_OUT, GRB > (data, n);
    FastLED.showColor(CRGB::Black);
    return int_count;
}

static int ring_autodetect(int pin_out, int pin_in, struct CRGB *pixels, int maxLeds)
{
    // binary search to find the led ring size
    int low = 1;
    int high = maxLeds + 1;
    pinMode(pin_in, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(pin_in), led_in_interrupt, FALLING);
    while ((high - low) > 1) {
        int n = (low + high) / 2;
        int overflow = ring_probe(pin_out, pixels, n);
        if (overflow) {
            high = n;
        } else {
            low = n;
        }
    }
    detachInterrupt(digitalPinToInterrupt(pin_in));

    // leave initialized
    ring_probe(pin_out, pixels, low);
    return low;
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
    Serial.begin(115200);
    printf("\nPOWERLIGHT\n");

    EditInit(line, sizeof(line));
    snprintf(espid, sizeof(espid), "esp8266-powerlight-%06x", ESP.getChipId());

    // autodetect the ring size, then show a colour gradient
    num_pixels = ring_autodetect(PIN_LED_OUT, PIN_LED_IN, ledring, MAX_LEDS);
    printf("Detected %d-pixel ring\n", num_pixels);
    for (int i = 0; i < 256; i++) {
        int n = num_pixels * i / 256;
        ledring[n] = CHSV(i, 255, 255);
    }
    FastLED.setBrightness(32);
    FastLED.show();

    // connect to wifi
    printf("Starting WIFI manager (%s)...\n", WiFi.SSID().c_str());
    wifiManager.setConfigPortalTimeout(120);
    wifiManager.autoConnect("ESP-POWERLIGHT");
}

void loop(void)
{
    // fetch a new value every POLL_INTERVAL
    static unsigned int period_last = -1;
    unsigned int period = millis() / POLL_INTERVAL;
    if (period != period_last) {
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
}
