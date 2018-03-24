/* uncomment if the default 4 bit mode doesn't work */
/* ------------------------------------------------ */
// #define BOARD_HAS_1BIT_SDMMC true // forces 1bit mode for SD MMC
/* ------------------------------------------------ */

#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string>
#include <cstddef>
#include <Wire.h>
#include <Preferences.h>
#include <M5Stack.h>             // https://github.com/m5stack/M5Stack/
#include "M5StackUpdater.h"      // https://github.com/tobozo/M5Stack-SD-Updater

using namespace std;

/* ===== compile settings ===== */
#define MAX_CH 14       // 1 - 14 channels (1-11 for US, 1-13 for EU and 1-14 for Japan)
#define SNAP_LEN 2324   // max len of each recieved packet

//#define USE_SD
#ifdef USE_SD
  #define BUTTON_PIN 5    // button to change the channel
#endif

#define USE_TFT
//#define USE_DISPLAY     // comment out if you don't want to use the OLED display
#define FLIP_DISPLAY    // comment out if you don't like to flip it
#define SDA_PIN 26
#define SCL_PIN 27
#define MAX_X 320
#define MAX_Y 220

#if CONFIG_FREERTOS_UNICORE
  #define RUNNING_CORE 0
#else
  #define RUNNING_CORE 1
#endif

#ifdef USE_DISPLAY
  #include "SH1106.h"
#endif

#include "FS.h"
#include "SD_MMC.h"
#include "Buffer.h"

esp_err_t event_handler(void* ctx, system_event_t* event) {
  return ESP_OK;
}

/* ===== run-time variables ===== */
Buffer sdBuffer;
#ifdef USE_DISPLAY
  SH1106 display(0x3c, SDA_PIN, SCL_PIN);
#endif
Preferences preferences;

bool useSD = false;
bool buttonPressed = false;
bool buttonEnabled = true;
uint32_t lastDrawTime;
uint32_t lastButtonTime;
uint32_t tmpPacketCounter;
uint32_t pkts[MAX_X];       // here the packets per second will be saved
uint32_t deauths = 0;       // deauth frames per second
unsigned int ch = 1;        // current 802.11 channel
int rssiSum;

/* ===== functions ===== */
double getMultiplicator() {
  uint32_t maxVal = 1;
  for (int i = 0; i < MAX_X; i++) {
    if (pkts[i] > maxVal) maxVal = pkts[i];
  }
  if (maxVal > MAX_Y) return (double)MAX_Y / (double)maxVal;
  else return 1;
}

void setChannel(int newChannel) {
  ch = newChannel;
  if (ch > MAX_CH || ch < 1) ch = 1;

  preferences.begin("packetmonitor32", false);
  preferences.putUInt("channel", ch);
  preferences.end();

  esp_wifi_set_promiscuous(false);
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous_rx_cb(&wifi_promiscuous);
  esp_wifi_set_promiscuous(true);
}

#ifdef USE_SD
  bool setupSD() {
    if (!SD_MMC.begin()) {
      Serial.println("Card Mount Failed");
      return false;
    }
  
    uint8_t cardType = SD_MMC.cardType();
  
    if (cardType == CARD_NONE) {
      Serial.println("No SD_MMC card attached");
      return false;
    }
  
    Serial.print("SD_MMC Card Type: ");
    if (cardType == CARD_MMC) {
      Serial.println("MMC");
    } else if (cardType == CARD_SD) {
      Serial.println("SDSC");
    } else if (cardType == CARD_SDHC) {
      Serial.println("SDHC");
    } else {
      Serial.println("UNKNOWN");
    }
  
    uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
    Serial.printf("SD_MMC Card Size: %lluMB\n", cardSize);
  
    return true;
  }
#endif

void wifi_promiscuous(void* buf, wifi_promiscuous_pkt_type_t type) {
  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  wifi_pkt_rx_ctrl_t ctrl = (wifi_pkt_rx_ctrl_t)pkt->rx_ctrl;

  if (type == WIFI_PKT_MGMT && (pkt->payload[0] == 0xA0 || pkt->payload[0] == 0xC0 )) deauths++;

  if (type == WIFI_PKT_MISC) return;             // wrong packet type
  if (ctrl.sig_len > SNAP_LEN) return;           // packet too long

  uint32_t packetLength = ctrl.sig_len;
  if (type == WIFI_PKT_MGMT) packetLength -= 4;  // fix for known bug in the IDF https://github.com/espressif/esp-idf/issues/886

  //Serial.print(".");
  tmpPacketCounter++;
  rssiSum += ctrl.rssi;

  if (useSD) sdBuffer.addPacket(pkt->payload, packetLength);
}

void draw() {
  #ifdef USE_DISPLAY
    double multiplicator = getMultiplicator();
    int len;
    int rssi;
  
    if (pkts[MAX_X - 1] > 0) rssi = rssiSum / (int)pkts[MAX_X - 1];
    else rssi = rssiSum;
  
    display.clear();
  
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString( 10, 0, (String)ch);
    display.drawString( 14, 0, ("|"));
    display.drawString( 30, 0, (String)rssi);
    display.drawString( 34, 0, ("|"));
    display.drawString( 82, 0, (String)tmpPacketCounter);
    display.drawString( 87, 0, ("["));
    display.drawString(106, 0, (String)deauths);
    display.drawString(110, 0, ("]"));
    display.drawString(114, 0, ("|"));
    display.drawString(128, 0, (useSD ? "SD" : ""));
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString( 36,  0, ("Pkts:"));
  
    display.drawLine(0, 63 - MAX_Y, MAX_X, 63 - MAX_Y);
    for (int i = 0; i < MAX_X; i++) {
      len = pkts[i] * multiplicator;
      display.drawLine(i, 63, i, 63 - (len > MAX_Y ? MAX_Y : len));
      if (i < MAX_X - 1) pkts[i] = pkts[i + 1];
    }
    display.display();
  #endif

  #ifdef USE_TFT
    double multiplicator = getMultiplicator();
    int len;
    int rssi;
  
    if (pkts[MAX_X - 1] > 0) rssi = rssiSum / (int)pkts[MAX_X - 1];
    else rssi = rssiSum;
    
    for (int i = 0; i < MAX_X; i++) {
      len = pkts[i] * multiplicator;
      if (i < MAX_X - 1) {
        pkts[i] = pkts[i + 1];
        int newlen = pkts[i] * multiplicator;
        int oldpos = 239 - (len > MAX_Y ? MAX_Y : len);
        int newpos = 239 - (newlen > MAX_Y ? MAX_Y : newlen);
        if(newlen-len!=0) {
          if(newlen>len) {
            M5.Lcd.drawLine(i, oldpos,  i, newpos, WHITE);      
          } else {
            M5.Lcd.drawLine(i, oldpos,  i, newpos, BLACK);      
          }
        }
      }
    }
  
    M5.Lcd.setTextColor(WHITE, BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(0, 10);
    M5.Lcd.print(" Ch: ");
    M5.Lcd.println(ch);
    M5.Lcd.print(" RSSI: ");
    M5.Lcd.println(rssi);
    M5.Lcd.print(" Pkts: ");
    M5.Lcd.println(tmpPacketCounter);
    M5.Lcd.print(" Deauths: ");
    M5.Lcd.println(deauths);
  #endif

}

/* ===== main program ===== */
void setup() {

  // Serial
  Serial.begin(115200);
  Serial.println("Begin...");
  M5.begin();
  Wire.begin();
  if(digitalRead(BUTTON_A_PIN) == 0) {
    Serial.println("Will Load menu binary");
    updateFromFS(SD,"/menu.bin");
    ESP.restart();
  }
  
  // Settings
  preferences.begin("packetmonitor32", false);
  ch = preferences.getUInt("channel", 1);
  preferences.end();

  // System & WiFi
  nvs_flash_init();
  tcpip_adapter_init();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  //ESP_ERROR_CHECK(esp_wifi_set_country(WIFI_COUNTRY_EU));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
  ESP_ERROR_CHECK(esp_wifi_start());

  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

  #ifdef USE_SD
    // SD card
    sdBuffer = Buffer();
  
    if (setupSD())
      sdBuffer.open(&SD_MMC);
  
    // I/O
    pinMode(BUTTON_PIN, INPUT_PULLUP);
  #endif

  #ifdef USE_TFT
    M5.Lcd.setBrightness(100);
    M5.Lcd.fillScreen(BLACK);
  #endif

  // display
  #ifdef USE_DISPLAY
    display.init();
    #ifdef FLIP_DISPLAY
      display.flipScreenVertically();
    #endif
  
    /* show start screen */
    display.clear();
    display.setFont(ArialMT_Plain_16);
    display.drawString(6, 6, "PacketMonitor32");
    display.setFont(ArialMT_Plain_10);
    display.drawString(24, 34, "Made with <3 by");
    display.drawString(29, 44, "@Spacehuhn");
    display.display();
  
    delay(1000);
  #endif

  // second core
  xTaskCreatePinnedToCore(
    coreTask,               /* Function to implement the task */
    "coreTask",             /* Name of the task */
    2500,                   /* Stack size in words */
    NULL,                   /* Task input parameter */
    0,                      /* Priority of the task */
    NULL,                   /* Task handle. */
    RUNNING_CORE);          /* Core where the task should run */

  // start Wifi sniffer
  esp_wifi_set_promiscuous_rx_cb(&wifi_promiscuous);
  esp_wifi_set_promiscuous(true);
}


void loop() {
  vTaskDelay(portMAX_DELAY);
}


void coreTask( void * p ) {

  uint32_t currentTime;

  while (true) {

    currentTime = millis();

    /* bit of spaghetti code, have to clean this up later :D */
    #ifdef USE_SD
      // check button
      if (digitalRead(BUTTON_PIN) == LOW) {
        if (buttonEnabled) {
          if (!buttonPressed) {
            buttonPressed = true;
            lastButtonTime = currentTime;
          } else if (currentTime - lastButtonTime >= 2000) {
            if (useSD) {
              useSD = false;
              sdBuffer.close(&SD_MMC);
              draw();
            } else {
              if (setupSD())
                sdBuffer.open(&SD_MMC);
              draw();
            }
            buttonPressed = false;
            buttonEnabled = false;
          }
        }
      } else {
        if (buttonPressed) {
          setChannel(ch + 1);
          draw();
        }
        buttonPressed = false;
        buttonEnabled = true;
      }
  
      // save buffer to SD
      if (useSD)
        sdBuffer.save(&SD_MMC);

    #endif
    
    // draw Display
    if ( currentTime - lastDrawTime > 1000 ) {
      lastDrawTime = currentTime;
      // Serial.printf("\nFree RAM %u %u\n", heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT), heap_caps_get_minimum_free_size(MALLOC_CAP_32BIT));// for debug purposes

      pkts[MAX_X - 1] = tmpPacketCounter;

      draw();

      Serial.println((String)pkts[MAX_X - 1]);

      tmpPacketCounter = 0;
      deauths = 0;
      rssiSum = 0;
    }

    // Serial input
    if (Serial.available()) {
      ch = Serial.readString().toInt();
      if (ch < 1 || ch > 14) ch = 1;
      setChannel(ch);
      Serial.printf("Setting channel %02d\n", ch);
    }
  }
}
