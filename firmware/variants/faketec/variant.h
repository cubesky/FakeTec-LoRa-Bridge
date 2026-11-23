/*
 * FakeTec v5 — nice!nano / ProMicro nRF52840 variant.
 * Arduino pin number == nRF GPIO number (P0.n = n, P1.n = 32 + n).
 */
#ifndef _VARIANT_FAKETEC_
#define _VARIANT_FAKETEC_

#define VARIANT_MCK (64000000ul)
#define USE_LFXO

#include "WVariant.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PINS_COUNT         (48)
#define NUM_DIGITAL_PINS   (48)
#define NUM_ANALOG_INPUTS  (8)
#define NUM_ANALOG_OUTPUTS (0)

// LED (nice!nano on-board blue LED, P0.15)
#define PIN_LED1      (15)
#define LED_BUILTIN   PIN_LED1
#define LED_CONN      PIN_LED1
#define LED_RED       PIN_LED1
#define LED_BLUE      PIN_LED1
#define LED_STATE_ON  1

// User button SW2 (P1.00, "D6"), active low, external 10k pull-up
#define PIN_BUTTON1   (32 + 0)

// Analog pins (P0.02 .. P0.31 SAADC capable)
#define PIN_A0 (2)
#define PIN_A1 (3)
#define PIN_A2 (4)
#define PIN_A3 (5)
#define PIN_A4 (28)
#define PIN_A5 (29)
#define PIN_A6 (30)
#define PIN_A7 (31)
static const uint8_t A0 = PIN_A0;
static const uint8_t A1 = PIN_A1;
static const uint8_t A2 = PIN_A2;
static const uint8_t A3 = PIN_A3;
static const uint8_t A4 = PIN_A4;
static const uint8_t A5 = PIN_A5;
static const uint8_t A6 = PIN_A6;
static const uint8_t A7 = PIN_A7;
#define ADC_RESOLUTION 14

#define PIN_VBAT      (31)  // battery divider 10M/10M, "Batt%"

// NFC pins are used as GPIO (LoRa RST / DIO1); see CONFIG_NFCT_PINS_AS_GPIOS
#define PIN_NFC1 (9)
#define PIN_NFC2 (10)

// Serial1 on the GPS header (J14)
#define PIN_SERIAL1_RX (22)
#define PIN_SERIAL1_TX (20)

// SPI — LoRa module
#define SPI_INTERFACES_COUNT 1
#define PIN_SPI_MISO (2)
#define PIN_SPI_MOSI (32 + 15)
#define PIN_SPI_SCK  (32 + 11)
static const uint8_t SS   = (32 + 13);
static const uint8_t MOSI = PIN_SPI_MOSI;
static const uint8_t MISO = PIN_SPI_MISO;
static const uint8_t SCK  = PIN_SPI_SCK;

// I2C — OLED
#define WIRE_INTERFACES_COUNT 1
#define PIN_WIRE_SDA (32 + 4)
#define PIN_WIRE_SCL (11)

#ifdef __cplusplus
}
#endif

#endif
