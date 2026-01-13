#include <FS.h> // For LittleFS
#include <LittleFS.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h> 
#include <ArduinoJson.h> 
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecure.h>
#include <ThingSpeak.h>
#include <NTPClient.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include <DHT.h>
#include <math.h>
#include "secrets.h"

// --- SAFE VARIABLES (No Secrets Here!) ---
char thingSpeakApiKey[20] = "";
char thingSpeakChannelId[15] = "";

// Flags
bool shouldSaveConfig = false;

// --- CONFIGURATION ---
const float TEMP_OFFSET = 0; 
#define HISTORY_SIZE 18         
#define TIME_OFFSET 19800      // GMT+5:30

// --- OFFLINE BUFFER ---
struct WeatherData {
  float temp; float dhtTemp; float hum; float press;
  int zambretti; float absHum; float dewPoint;
};
#define BUFFER_SIZE 150
WeatherData offlineBuffer[BUFFER_SIZE];
int bufferCount = 0; 

// --- HARDWARE OBJECTS ---
#define DHTPIN 14       
#define DHTTYPE DHT11  
DHT dht(DHTPIN, DHTTYPE);
Adafruit_BMP280 bmp;   

WiFiClient client;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", TIME_OFFSET);

// --- GLOBAL VARIABLES ---
float pressureHistory[HISTORY_SIZE];
int histIndex = 0;
bool histFull = false;
double tempSum = 0; double pressSum = 0; int sampleCount = 0;
float currentHum = 0; float currentDhtTemp = 0;
int wifiStrength = 0; 

// Timers
unsigned long lastSecond = 0;
unsigned long lastSlowScan = 0;
unsigned long lastLog = 0;
unsigned long lastHistory = 0;
unsigned long lastOTACheck = 0;

// CPU Overclocking
extern "C" { 
  #include "user_interface.h"
   }

// --- CALLBACK: Save Config ---
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

// --- FUNCTION PROTOTYPES ---
void uploadToThingSpeak(float bmpVal, float dhtVal, float h, float p, int z, float ah, float dp, int wifiRssi, int secondsAgo);
int calculateZambretti(float currentP);
void checkForOTAUpdate();

// --- SETUP ---
void setup() {
  system_update_cpu_freq(160); 
  Serial.begin(115200);
  delay(1000); 
  Serial.println("\n\n--- BOOTING WEATHER SENTINEL (PRO) ---");

  // 1. MOUNT FILE SYSTEM (To Read Saved Keys)
  Serial.println("Mounting FS...");
  if (LittleFS.begin()) {
    Serial.println("Mounted file system");
    if (LittleFS.exists("/config.json")) {
      // File exists, reading and loading
      Serial.println("Reading config file");
      File configFile = LittleFS.open("/config.json", "r");
      if (configFile) {
        size_t size = configFile.size();
        std::unique_ptr<char[]> buf(new char[size]);
        configFile.readBytes(buf.get(), size);
        
        JsonDocument json;
        DeserializationError error = deserializeJson(json, buf.get());
        if (!error) {
          Serial.println("\nParsed JSON");
          strcpy(thingSpeakApiKey, json["api_key"]);
          strcpy(thingSpeakChannelId, json["channel_id"]);
        } else {
          Serial.println("Failed to load json config");
        }
        configFile.close();
      }
    }
  } else {
    Serial.println("Failed to mount FS");
  }

  // 2. WIFI MANAGER SETUP
  WiFiManager wm;
  wm.setSaveConfigCallback(saveConfigCallback);

  // Custom Text Boxes for Portal
  WiFiManagerParameter custom_api_key("apikey", "ThingSpeak Write Key", thingSpeakApiKey, 20);
  WiFiManagerParameter custom_channel_id("channelid", "Channel ID", thingSpeakChannelId, 15);

  wm.addParameter(&custom_api_key);
  wm.addParameter(&custom_channel_id);

  // 3. CONNECT OR CREATE HOTSPOT
  // If it can't connect, it creates "Weather-Setup" with password "password123"
  if (!wm.autoConnect("Weather-Setup", "password123")) {
    Serial.println("Failed to connect and hit timeout");
    delay(3000);
    ESP.reset();
    delay(5000);
  }

  Serial.println("\nWiFi Connected!");
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  // 4. SAVE NEW CONFIG (If changed)
  strcpy(thingSpeakApiKey, custom_api_key.getValue());
  strcpy(thingSpeakChannelId, custom_channel_id.getValue());

  if (shouldSaveConfig) {
    Serial.println("Saving config");
    JsonDocument json;
    json["api_key"] = thingSpeakApiKey;
    json["channel_id"] = thingSpeakChannelId;

    File configFile = LittleFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("Failed to open config file for writing");
    }
    serializeJson(json, configFile);
    configFile.close();
    Serial.println("Config Saved.");
  }

  // 5. HARDWARE & TIME INIT
  dht.begin();
  if (!bmp.begin(0x76)) { if (!bmp.begin(0x77)) Serial.println("Error: BMP280 missing"); }
  
  timeClient.begin();
  timeClient.update();
  ThingSpeak.begin(client);

  // 6. OTA SETUP
  ArduinoOTA.setHostname("WeatherSentinel-Pro");
  ArduinoOTA.setPassword("admin123"); 
  ArduinoOTA.begin();

  // 7. PRINT CURRENT FIRMWARE VERSION
  Serial.print("Current Firmware Version: ");
  Serial.println(FIRMWARE_VERSION);

  // Reset timers
  lastLog = millis() - 300000;      
  lastSlowScan = millis() - 30000;
  lastHistory = millis() - 600000;
  lastOTACheck = millis() - 3600000; // Check for updates 1 hour after boot
}

// --- MAIN LOOP ---
void loop() {
  ArduinoOTA.handle(); 

  unsigned long now = millis();

  // Fast Sample (1s)
  if (now - lastSecond >= 1000) {
    lastSecond = now;
    tempSum += bmp.readTemperature();
    pressSum += (bmp.readPressure() / 100.0F); 
    sampleCount++;
  }

  // Slow Sample (30s)
  if (now - lastSlowScan >= 30000) {
    lastSlowScan = now;
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (!isnan(h)) { currentHum = h; currentDhtTemp = t; }
    wifiStrength = WiFi.RSSI(); // Get WiFi signal strength
  }

  // History (10m)
  if (now - lastHistory >= 600000) {
    lastHistory = now;
    pressureHistory[histIndex] = bmp.readPressure() / 100.0F;
    histIndex = (histIndex + 1) % HISTORY_SIZE;
    if (histIndex == 0) histFull = true; 
  }

  // OTA Update Check (1 hour)
  if (now - lastOTACheck >= 3600000) {
    lastOTACheck = now;
    if (WiFi.status() == WL_CONNECTED) {
      checkForOTAUpdate();
    }
  }

  // Upload (5m)
  if (now - lastLog >= 300000) {
    lastLog = now;
    if (sampleCount > 0) {
      float avgTemp = (tempSum / sampleCount) + TEMP_OFFSET;
      float avgPress = pressSum / sampleCount;
      float safeHum = (currentHum == 0) ? 1.0 : currentHum; 
      float dewPoint = avgTemp - ((100 - safeHum) / 5.0);
      float absHum = (6.112 * exp((17.67 * avgTemp)/(avgTemp+243.5)) * safeHum * 2.1674) / (273.15 + avgTemp);
      int forecastCode = calculateZambretti(avgPress);

      if (WiFi.status() == WL_CONNECTED) {
          timeClient.update();
          uploadToThingSpeak(avgTemp, currentDhtTemp, currentHum, avgPress, forecastCode, absHum, dewPoint, wifiStrength, 0);

          if (bufferCount > 0) {
             for (int i = bufferCount - 1; i >= 0; i--) {
                 unsigned long startWait = millis();
                 while(millis() - startWait < 16000) { ArduinoOTA.handle(); yield(); }
                 
                 WeatherData old = offlineBuffer[i];
                 int lagSeconds = (bufferCount - i) * 300;
                 uploadToThingSpeak(old.temp, old.dhtTemp, old.hum, old.press, old.zambretti, old.absHum, old.dewPoint, wifiStrength, lagSeconds);
                 bufferCount--;
             }
          }
      } else {
          // Offline Save
          if (bufferCount < BUFFER_SIZE) {
             offlineBuffer[bufferCount] = {avgTemp, currentDhtTemp, currentHum, avgPress, forecastCode, absHum, dewPoint};
             bufferCount++;
          } else {
             for (int i = 0; i < BUFFER_SIZE - 1; i++) offlineBuffer[i] = offlineBuffer[i+1];
             offlineBuffer[BUFFER_SIZE - 1] = {avgTemp, currentDhtTemp, currentHum, avgPress, forecastCode, absHum, dewPoint};
             bufferCount = BUFFER_SIZE;
          }
      }
      tempSum = 0; pressSum = 0; sampleCount = 0;
    }
  }
}

// --- HELPER FUNCTION ---
void uploadToThingSpeak(float bmpVal, float dhtVal, float h, float p, int z, float ah, float dp, int wifiRssi, int secondsAgo) {
     ThingSpeak.setField(1, bmpVal);      
     ThingSpeak.setField(2, h);           
     ThingSpeak.setField(3, p);           
     ThingSpeak.setField(4, z);           
     ThingSpeak.setField(5, ah);          
     ThingSpeak.setField(6, dp);          
     ThingSpeak.setField(7, dhtVal);      
     ThingSpeak.setField(8, wifiRssi);      

     if (secondsAgo > 0) {
        unsigned long nowEpoch = timeClient.getEpochTime();
        time_t rawTime = (time_t)(nowEpoch - secondsAgo);
        struct tm * ti = localtime(&rawTime);
        char timeString[25];
        sprintf(timeString, "%d-%02d-%02d %02d:%02d:%02d", 
                ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday, ti->tm_hour, ti->tm_min, ti->tm_sec);
        ThingSpeak.setCreatedAt(timeString);
     }
     
     // CONVERT Char Array to Long for ThingSpeak
     unsigned long channelNum = atol(thingSpeakChannelId);
     ThingSpeak.writeFields(channelNum, thingSpeakApiKey);
     
     // Simple Success Check
     Serial.println("Data sent to ThingSpeak (Check Dashboard)");
}

int calculateZambretti(float currentP) {
  if (!histFull) return 0; 
  float delta = currentP - pressureHistory[histIndex];
  if (delta < -1.6) return 1; 
  if (currentP < 1000) return 2; 
  if (delta > 1.6) return 4; 
  return 3; 
}

void checkForOTAUpdate() {
  Serial.println("\n--- Checking for OTA Updates ---");
  
  WiFiClientSecure client;
  client.setInsecure(); // For GitHub HTTPS
  
  HTTPClient http;
  
  // Build GitHub raw URL for version file
  String versionURL = "https://raw.githubusercontent.com/" + String(GITHUB_USER) + "/" + 
                      String(GITHUB_REPO) + "/" + String(GITHUB_BRANCH) + "/version.txt";
  
  Serial.print("Version URL: ");
  Serial.println(versionURL);
  
  http.begin(client, versionURL);
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String newVersion = http.getString();
    newVersion.trim();
    
    Serial.print("Current Version: ");
    Serial.println(FIRMWARE_VERSION);
    Serial.print("Available Version: ");
    Serial.println(newVersion);
    
    if (newVersion != String(FIRMWARE_VERSION)) {
      Serial.println("New firmware available! Starting update...");
      
      // Build firmware binary URL
      String firmwareURL = "https://raw.githubusercontent.com/" + String(GITHUB_USER) + "/" + 
                          String(GITHUB_REPO) + "/" + String(GITHUB_BRANCH) + "/firmware.bin";
      
      Serial.print("Firmware URL: ");
      Serial.println(firmwareURL);
      
      // Perform OTA update
      t_httpUpdate_return ret = ESPhttpUpdate.update(client, firmwareURL);
      
      switch(ret) {
        case HTTP_UPDATE_FAILED:
          Serial.printf("Update failed. Error (%d): %s\n", 
                       ESPhttpUpdate.getLastError(), 
                       ESPhttpUpdate.getLastErrorString().c_str());
          break;
          
        case HTTP_UPDATE_NO_UPDATES:
          Serial.println("No update available.");
          break;
          
        case HTTP_UPDATE_OK:
          Serial.println("Update successful! Rebooting...");
          break;
      }
    } else {
      Serial.println("Firmware is up to date.");
    }
  } else {
    Serial.printf("Version check failed. HTTP Code: %d\n", httpCode);
  }
  
  http.end();
}