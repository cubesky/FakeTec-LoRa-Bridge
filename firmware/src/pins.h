#pragma once
// FakeTec v5 pin assignment (from faketec.kicad_sch, nice!nano footprint).
// Pin number = port * 32 + pin.

#define P0_(n) (n)
#define P1_(n) (32 + (n))

// LoRa — Ra-01S (SX1268 + TCXO) / HT-RA62 / Ra-01SH (SX1262)
#define PIN_LORA_CS     P1_(13)
#define PIN_LORA_SCK    P1_(11)
#define PIN_LORA_MOSI   P1_(15)
#define PIN_LORA_MISO   P0_(2)
#define PIN_LORA_RST    P0_(9)
#define PIN_LORA_DIO1   P0_(10)
#define PIN_LORA_BUSY   P0_(29)
#define PIN_LORA_RXEN   P0_(17)   // RX switch; TX switch driven by DIO2

// I2C OLED (J5)
#define PIN_OLED_SDA    P1_(4)
#define PIN_OLED_SCL    P0_(11)

// Misc
#define PIN_BATT_ADC    P0_(31)   // AIN7, divider 10M / 10M
#define BATT_DIVIDER    2.0f
#define PIN_BUTTON      P1_(0)    // SW2, active low
#define PIN_LED         P0_(15)   // nice!nano blue LED
#define PIN_EXT_VCC_EN  P0_(13)   // nice!nano: HIGH = VCC (3.3V rail for LoRa/OLED) on

// MOSFET outputs T1..T3 (SI2312 low-side switches, gate pull-down)
#define PIN_T1G         P0_(24)
#define PIN_T2G         P0_(8)
#define PIN_T3G         P0_(6)
