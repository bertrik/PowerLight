#include <Arduino.h>

#include "WiFiManager.h"
#include "FastLED.h"

#define printf Serial.printf
#define POLL_INTERVAL   60000L
#define NUM_LEDS        40

#define DATA_PIN_LED    D1

static WiFiManager wifiManager;
static CRGB ledring[NUM_LEDS];

void setup(void)
{
    Serial.begin(115200);
    printf("\nPOWERLIGHT\n");

    // configure LED ring    
    FastLED.addLeds < WS2812B, DATA_PIN_LED, RGB > (ledring, NUM_LEDS).setCorrection(TypicalSMD5050);

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

        // fetch JSON with fractions
        // ...

        // draw it on the ring
        // ...

    }
}
