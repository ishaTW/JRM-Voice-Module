#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <HardwareSerial.h>
#include <TinyGPS++.h>
#include "BluetoothSerial.h"
#include <Preferences.h>

// ESP8266Audio Libraries (Required for MP3 decoding and I2S output)
#include "AudioFileSourceSD.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

/* =========================================================================
 * HARDWARE CONNECTIONS & PIN CONFIGURATION
 * =========================================================================
 * 1. GPS Module (L80) -> ESP32
 *    - GPS VCC -> ESP32 5V (or 3.3V depending on module)
 *    - GPS GND -> ESP32 GND
 *    - GPS TX  -> ESP32 GPIO 34 (RX1)
 *    - GPS RX  -> ESP32 GPIO 4 (TX1)
 *
 * 2. SD Card Module -> ESP32
 *    - VCC  -> 3.3V
 *    - GND  -> GND
 *    - MISO -> GPIO 19
 *    - MOSI -> GPIO 23
 *    - SCK  -> GPIO 18
 *    - CS   -> GPIO 5
 *
 * 3. MAX98357 I2S DAC -> ESP32
 *    - VIN  -> 5V
 *    - GND  -> GND
 *    - BCLK -> GPIO 32
 *    - LRC  -> GPIO 33 (Word Select)
 *    - DIN  -> GPIO 22 (Data In)
 *
 * 4. Buzzer -> ESP32
 *    - VCC  -> GPIO 14
 *    - GND  -> ESP32 GND
 *
 * 5. LED's -> ESP32
 *    - Red LED (+ve)    -> GPIO 25
 *    - Yellow LED (+ve) -> GPIO 26
 *    - Green LED (+ve)  -> GPIO 27
 *    - LED's (-ve)      -> ESP32 GND
 *
 * 6. Push Button -> ESP32
 *    - Pin 1 -> GPIO 13
 *    - Pin 2 -> ESP32 GND (Configured with INPUT_PULLUP)
 * ========================================================================= */

// LED Pins
const int pinRedLED = 25;
const int pinYellowLED = 26;
const int pinGreenLED = 27;

// Buzzer Pin
const int pinBuzzer = 14;

// Button Pin (Active LOW with internal pull-up)
const int pinButton = 13;

// GPS Port (Hardware Serial 1)
HardwareSerial GPSSerial(1);
TinyGPSPlus gps;

// Bluetooth Serial
BluetoothSerial SerialBT;

// Non-Volatile Storage (Preferences)
Preferences prefs;

// Audio Objects
AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSourceSD *audioFile = nullptr;
AudioOutputI2S *i2sOutput = nullptr;

// System States
enum SystemState {
    STATE_LANG_SELECT,
    STATE_ROUTE_SELECT,
    STATE_WAITING_GPS,
    STATE_RUNNING
};
SystemState currentState = STATE_LANG_SELECT;

// Language States
enum Language {
    LANG_ENGLISH = 0,
    LANG_HINDI = 1,
    LANG_MARATHI = 2
};
Language currentLang = LANG_ENGLISH;

// Route coordinate structure
struct RoutePoint {
    double lat;
    double lon;
    char zone; // 'G' = Green, 'Y' = Yellow, 'R' = Red
};

// Route Storage (Max 500 coordinates)
const int MAX_ROUTE_POINTS = 500;
RoutePoint routePoints[MAX_ROUTE_POINTS];
int routePointCount = 0;
String currentRouteName = "";

// Simulated Override Flags (For testing without GPS / indoor)
bool isSpeedSimulated = false;
double simulatedSpeedKmh = 0.0;
bool isLocationSimulated = false;
double simulatedLat = 0.0;
double simulatedLon = 0.0;
bool isZoneManualOverridden = false;
char manualOverriddenZone = ' ';

// Monitoring variables
char currentZone = ' '; // 'G', 'Y', 'R', or ' ' (none)
bool isOverspeeding = false;
unsigned long lastStatusPrintTime = 0;
unsigned long gpsLockStartTime = 0;

// Button Debounce and Language Selection Logic
int buttonPressCount = 0;
unsigned long lastButtonPressTime = 0;
bool lastButtonState = HIGH;
bool buttonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

// Non-blocking Buzzer State
struct BuzzerConfig {
    bool active;
    int beepCount;
    unsigned long lastToggleTime;
    bool pinState;
    int maxBeeps;
    unsigned long durationOn;
    unsigned long durationOff;
} buzzer = {false, 0, 0, false, 3, 150, 150};

/* =========================================================================
 * AUDIO PLAYER FUNCTIONS
 * ========================================================================= */

void setupAudio() {
    i2sOutput = new AudioOutputI2S();
    i2sOutput->SetPinout(32, 33, 22);
    i2sOutput->SetGain(1.0);
    mp3 = new AudioGeneratorMP3();
}

void stopAudio() {
    if (mp3 && mp3->isRunning()) {
        mp3->stop();
    }
    if (audioFile) {
        delete audioFile;
        audioFile = nullptr;
    }
}

void playAudio(const char *filename) {
    stopAudio();
    audioFile = new AudioFileSourceSD(filename);
    if (audioFile && audioFile->isOpen()) {
        Serial.printf("[AUDIO] Playing: %s\n", filename);
        SerialBT.printf("[AUDIO] Playing: %s\n", filename);
        mp3->begin(audioFile, i2sOutput);
    } else {
        Serial.printf("[AUDIO] Failed to open: %s\n", filename);
        SerialBT.printf("[AUDIO] Failed to open: %s\n", filename);
        if (audioFile) { delete audioFile; audioFile = nullptr; }
    }
}

void updateAudio() {
    if (mp3 && mp3->isRunning()) {
        if (!mp3->loop()) {
            mp3->stop();
            Serial.println("[AUDIO] Finished playing file.");
            SerialBT.println("[AUDIO] Finished playing file.");
        }
    }
}

/* =========================================================================
 * BUZZER FUNCTIONS
 * ========================================================================= */

void triggerBuzzer(int count = 3, unsigned long onTime = 150, unsigned long offTime = 150) {
    buzzer.active = true;
    buzzer.beepCount = 0;
    buzzer.maxBeeps = count;
    buzzer.durationOn = onTime;
    buzzer.durationOff = offTime;
    buzzer.lastToggleTime = millis();
    buzzer.pinState = true;
    digitalWrite(pinBuzzer, HIGH);
}

void updateBuzzer() {
    if (!buzzer.active) { digitalWrite(pinBuzzer, LOW); return; }
    unsigned long now = millis();
    unsigned long interval = buzzer.pinState ? buzzer.durationOn : buzzer.durationOff;
    if (now - buzzer.lastToggleTime >= interval) {
        buzzer.lastToggleTime = now;
        if (buzzer.pinState) {
            digitalWrite(pinBuzzer, LOW);
            buzzer.pinState = false;
            buzzer.beepCount++;
            if (buzzer.beepCount >= buzzer.maxBeeps) buzzer.active = false;
        } else {
            digitalWrite(pinBuzzer, HIGH);
            buzzer.pinState = true;
        }
    }
}

/* =========================================================================
 * BUTTON DEBOUNCE
 * ========================================================================= */

bool checkButtonPress() {
    int reading = digitalRead(pinButton);
    bool pressed = false;
    if (reading != lastButtonState) lastDebounceTime = millis();
    if ((millis() - lastDebounceTime) > debounceDelay) {
        if (reading != buttonState) {
            buttonState = reading;
            if (buttonState == LOW) pressed = true;
        }
    }
    lastButtonState = reading;
    return pressed;
}

/* =========================================================================
 * ROUTE LOADING FUNCTIONS
 * ========================================================================= */

// Helper function to convert NMEA DDMM.MMMMM format to Decimal Degrees
double nmeaToDecimal(double nmeaVal) {
    double absVal = fabs(nmeaVal);
    int degrees = (int)(absVal / 100.0);
    double minutes = absVal - (degrees * 100.0);
    double decimal = degrees + (minutes / 60.0);
    return (nmeaVal < 0.0) ? -decimal : decimal;
}

bool loadRouteFile(String filename) {
    if (!SD.exists(filename)) {
        Serial.printf("[ROUTE] Error: File %s does not exist on SD Card.\n", filename.c_str());
        SerialBT.printf("[ROUTE] Error: File %s does not exist on SD Card.\n", filename.c_str());
        return false;
    }
    File file = SD.open(filename);
    if (!file) {
        Serial.printf("[ROUTE] Error: Failed to open file %s.\n", filename.c_str());
        SerialBT.printf("[ROUTE] Error: Failed to open file %s.\n", filename.c_str());
        return false;
    }
    routePointCount = 0;
    int lineNum = 0;
    Serial.printf("[ROUTE] Reading %s...\n", filename.c_str());
    
    while (file.available() && routePointCount < MAX_ROUTE_POINTS) {
        String line = file.readStringUntil('\n');
        lineNum++;
        line.trim();
        
        // Strip UTF-8 BOM if present
        if (line.startsWith("\xEF\xBB\xBF")) {
            line = line.substring(3);
            line.trim();
        }
        
        if (line.length() == 0) continue;
        
        // Split line by '$' for multiple records per line
        int startIdx = 0;
        while (startIdx < line.length() && routePointCount < MAX_ROUTE_POINTS) {
            int dollarIdx = line.indexOf('$', startIdx);
            String record;
            if (dollarIdx == -1) {
                record = line.substring(startIdx);
                startIdx = line.length();
            } else {
                record = line.substring(startIdx, dollarIdx);
                startIdx = dollarIdx + 1;
            }
            record.trim();
            if (record.length() == 0) continue;
            
            // Check for #D: prefix
            if (!record.startsWith("#D:")) {
                continue; // Ignore non-data lines/comments
            }
            
            // Remove "#D:" prefix
            String data = record.substring(3);
            data.trim();
            
            // Parse comma-separated fields: Latitude,Longitude,Distance,Zone,VoiceID
            int firstComma = data.indexOf(',');
            if (firstComma == -1) continue;
            int secondComma = data.indexOf(',', firstComma + 1);
            if (secondComma == -1) continue;
            int thirdComma = data.indexOf(',', secondComma + 1);
            if (thirdComma == -1) continue;
            int fourthComma = data.indexOf(',', thirdComma + 1);
            
            String latStr = data.substring(0, firstComma);
            String lonStr = data.substring(firstComma + 1, secondComma);
            String zoneStr;
            if (fourthComma == -1) {
                zoneStr = data.substring(thirdComma + 1);
            } else {
                zoneStr = data.substring(thirdComma + 1, fourthComma);
            }
            
            latStr.trim();
            lonStr.trim();
            zoneStr.trim();
            
            if (latStr.length() == 0 || lonStr.length() == 0 || zoneStr.length() == 0) {
                continue;
            }
            
            double rawLat = latStr.toDouble();
            double rawLon = lonStr.toDouble();
            
            // Convert from NMEA format to Decimal Degrees
            double lat = nmeaToDecimal(rawLat);
            double lon = nmeaToDecimal(rawLon);
            char zone = zoneStr.charAt(0);
            
            if (zone == 'G' || zone == 'Y' || zone == 'R') {
                routePoints[routePointCount++] = {lat, lon, zone};
                Serial.printf(
                    "[ROUTE] %d -> %.6f , %.6f , %c\n",
                    routePointCount,
                    lat,
                    lon,
                    zone
                );
            } else {
                Serial.printf("[ROUTE-DEBUG] Line %d: Invalid zone '%c' in record '%s'\n", lineNum, zone, record.c_str());
            }
        }
    }
    file.close();
    Serial.printf("[ROUTE] Loaded %d coordinates from %s (Lines: %d).\n", routePointCount, filename.c_str(), lineNum);
    SerialBT.printf("[ROUTE] Loaded %d coordinates from %s (Lines: %d).\n", routePointCount, filename.c_str(), lineNum);
    return true;
}

/* =========================================================================
 * DISTANCE CALCULATION (HAVERSINE FORMULA)
 * ========================================================================= */

double calculateDistance(double lat1, double lon1, double lat2, double lon2) {
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    double rLat1 = lat1 * M_PI / 180.0;
    double rLat2 = lat2 * M_PI / 180.0;
    double a = sin(dLat/2.0)*sin(dLat/2.0) + sin(dLon/2.0)*sin(dLon/2.0)*cos(rLat1)*cos(rLat2);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return 6371000.0 * c;
}

/* =========================================================================
 * ZONE SOUND & LED ASSIGNMENT
 * ========================================================================= */

const char* getZoneAudioPath(char zone, Language lang) {
    if (lang == LANG_ENGLISH) {
        if (zone == 'G') return "/0001.mp3";
        if (zone == 'Y') return "/0002.mp3";
        if (zone == 'R') return "/0003.mp3";
    } else if (lang == LANG_HINDI) {
        if (zone == 'G') return "/0101.mp3";
        if (zone == 'Y') return "/0102.mp3";
        if (zone == 'R') return "/0103.mp3";
    } else if (lang == LANG_MARATHI) {
        if (zone == 'G') return "/0201.mp3";
        if (zone == 'Y') return "/0202.mp3";
        if (zone == 'R') return "/0203.mp3";
    }
    return "";
}

const char* getOverspeedAudioPath(Language lang) {
    if (lang == LANG_ENGLISH) return "/0004.mp3";
    if (lang == LANG_HINDI)   return "/0104.mp3";
    if (lang == LANG_MARATHI) return "/0204.mp3";
    return "";
}

void updateLEDs(char zone) {
    digitalWrite(pinRedLED,    zone == 'R' ? HIGH : LOW);
    digitalWrite(pinYellowLED, zone == 'Y' ? HIGH : LOW);
    digitalWrite(pinGreenLED,  zone == 'G' ? HIGH : LOW);
}

/* =========================================================================
 * COMMAND PARSER (SERIAL & BLUETOOTH)
 * ========================================================================= */

void processCommand(String cmd) {
    cmd.trim();
    if (cmd.length() == 0) return;

    Serial.printf("[COMMAND] Received: %s\n", cmd.c_str());
    SerialBT.printf("[COMMAND] Received: %s\n", cmd.c_str());

    // 1. Zone Manual overrides: G, Y, R
    if (cmd.equalsIgnoreCase("G")) {
        isZoneManualOverridden = true; manualOverriddenZone = 'G';
        currentZone = 'G';
        updateLEDs('G');
        const char* audioPath = getZoneAudioPath('G', currentLang);
        if (strlen(audioPath) > 0) playAudio(audioPath);
        Serial.println("[OVERRIDE] Manual zone override: GREEN forced.");
        SerialBT.println("[OVERRIDE] Manual zone override: GREEN forced.");
        if (currentState == STATE_ROUTE_SELECT || currentState == STATE_WAITING_GPS) currentState = STATE_RUNNING;
    }
    else if (cmd.equalsIgnoreCase("Y")) {
        isZoneManualOverridden = true; manualOverriddenZone = 'Y';
        currentZone = 'Y';
        updateLEDs('Y');
        const char* audioPath = getZoneAudioPath('Y', currentLang);
        if (strlen(audioPath) > 0) playAudio(audioPath);
        Serial.println("[OVERRIDE] Manual zone override: YELLOW forced.");
        SerialBT.println("[OVERRIDE] Manual zone override: YELLOW forced.");
        if (currentState == STATE_ROUTE_SELECT || currentState == STATE_WAITING_GPS) currentState = STATE_RUNNING;
    }
    else if (cmd.equalsIgnoreCase("R")) {
        isZoneManualOverridden = true; manualOverriddenZone = 'R';
        currentZone = 'R';
        updateLEDs('R');
        const char* audioPath = getZoneAudioPath('R', currentLang);
        if (strlen(audioPath) > 0) playAudio(audioPath);
        Serial.println("[OVERRIDE] Manual zone override: RED forced.");
        SerialBT.println("[OVERRIDE] Manual zone override: RED forced.");
        if (currentState == STATE_ROUTE_SELECT || currentState == STATE_WAITING_GPS) currentState = STATE_RUNNING;
    }
    // 2. Clear all overrides and return to automatic GPS
    else if (cmd.equalsIgnoreCase("GPS")) {
        isSpeedSimulated = false; isLocationSimulated = false; isZoneManualOverridden = false;
        Serial.println("[OVERRIDE] Simulation and overrides disabled. Tracking GPS location.");
        SerialBT.println("[OVERRIDE] Simulation and overrides disabled. Tracking GPS location.");
    }
    // 3. Coordinates override command (e.g. 19.0760,72.8777)
    else if (cmd.indexOf(',') != -1) {
        int commaIdx = cmd.indexOf(',');
        double lat = cmd.substring(0, commaIdx).toDouble();
        double lon = cmd.substring(commaIdx + 1).toDouble();
        if (lat != 0.0 && lon != 0.0) {
            isLocationSimulated = true; simulatedLat = lat; simulatedLon = lon;
            Serial.printf("[OVERRIDE] Simulated Position: Lat = %f, Lon = %f\n", simulatedLat, simulatedLon);
            SerialBT.printf("[OVERRIDE] Simulated Position: Lat = %f, Lon = %f\n", simulatedLat, simulatedLon);
            if (currentState == STATE_ROUTE_SELECT || currentState == STATE_WAITING_GPS) currentState = STATE_RUNNING;
        } else {
            Serial.println("[COMMAND] Invalid format. Use: latitude,longitude (e.g., 19.0760,72.8777)");
            SerialBT.println("[COMMAND] Invalid format. Use: latitude,longitude (e.g., 19.0760,72.8777)");
        }
    }
    // 4. Route name command: RouteXXX (e.g. Route500, Route510, Route520)
    //    Case-insensitive: route500 / ROUTE500 / Route500 all work
    
    else if (cmd.length() > 5 && cmd.substring(0, 5).equalsIgnoreCase("Route")) {
        String routeNum = cmd.substring(5);          // Strip "Route" prefix
        String filename  = "/Route" + routeNum + ".txt";
        if (SD.exists(filename)) {
            if (loadRouteFile(filename)) {
                currentRouteName = "Route" + routeNum;
                prefs.putString("route", routeNum); // Save route to memory
                currentState = STATE_WAITING_GPS;
                gpsLockStartTime = millis();
                Serial.printf("[STATE] Route '%s' loaded. Waiting for GPS lock...\n", currentRouteName.c_str());
                SerialBT.printf("[STATE] Route '%s' loaded. Waiting for GPS lock...\n", currentRouteName.c_str());
            }
        } else {
            Serial.printf("[ROUTE] Error: File %s not found on SD card!\n", filename.c_str());
            SerialBT.printf("[ROUTE] Error: File %s not found on SD card!\n", filename.c_str());
        }
    }
    // 5. Plain number: route by number (e.g. 500) or simulated speed (e.g. 80)
    else {
        double val = cmd.toDouble();
        if (val > 0.0 || cmd == "0") {
            String filename = "/Route" + cmd + ".txt";
            if (SD.exists(filename)) {
                if (loadRouteFile(filename)) {
                    currentRouteName = "Route" + cmd;
                    prefs.putString("route", cmd); // Save route to memory
                    currentState = STATE_WAITING_GPS;
                    gpsLockStartTime = millis();
                    Serial.printf("[STATE] Route '%s' loaded. Re-matching zone via GPS...\n", currentRouteName.c_str());
                    SerialBT.printf("[STATE] Route '%s' loaded. Re-matching zone via GPS...\n", currentRouteName.c_str());
                }
            } else {
                if (val > 150.0) {
                    Serial.printf("[ROUTE] Error: File %s not found on SD card! Cannot load route.\n", filename.c_str());
                    SerialBT.printf("[ROUTE] Error: File %s not found on SD card! Cannot load route.\n", filename.c_str());
                } else {
                    isSpeedSimulated = true; simulatedSpeedKmh = val;
                    Serial.printf("[OVERRIDE] Simulated Speed: %.2f km/h\n", simulatedSpeedKmh);
                    SerialBT.printf("[OVERRIDE] Simulated Speed: %.2f km/h\n", simulatedSpeedKmh);
                    if (currentState == STATE_ROUTE_SELECT) currentState = STATE_RUNNING;
                }
            }
        } else {
            Serial.println("[COMMAND] Unrecognized. Available: Route500/Route510/Route520, G, Y, R, GPS, <speed_kmh>, <lat,lon>");
            SerialBT.println("[COMMAND] Unrecognized. Available: Route500/Route510/Route520, G, Y, R, GPS, <speed_kmh>, <lat,lon>");
        }
    }
}

void checkSerialInput() {
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n'); cmd.trim();
        if (cmd.length() > 0) processCommand(cmd);
    }
    if (SerialBT.available()) {
        String cmd = SerialBT.readStringUntil('\n'); cmd.trim();
        if (cmd.length() > 0) processCommand(cmd);
    }
}

/* =========================================================================
 * GPS FAST LOCK SETUP
 * ========================================================================= */

void setupGPSFastLock() {
    delay(1000); // Give module time to boot completely
    
    // 1. Restrict NMEA output to only essential RMC + GGA sentences (reduces serial bottleneck)
    GPSSerial.println("$PMTK314,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0*28");
    delay(200);
    
    // 2. Set update rate to 1 Hz (1000ms) during lock search. 
    // High speeds like 5Hz during search can steal GPS CPU and make locking MUCH slower!
    GPSSerial.println("$PMTK220,1000*1F");
    delay(200);
    
    // 3. Enable EASY (Embedded Assist System) for faster TTFF on L80
    GPSSerial.println("$PMTK869,1,1*35");
    delay(200);
    
    // 4. Enable AIC (Active Interference Cancellation)
    GPSSerial.println("$PMTK286,1*23");
    delay(200);

    // 5. Issue Hot Start (forces module to use cached almanac)
    GPSSerial.println("$PMTK101*32");
    delay(200);
    
    Serial.println("[GPS] Fast lock configuration sent to module.");
}

/* =========================================================================
 * CORE SETUP & LOOP
 * ========================================================================= */

void setup() {
    Serial.begin(115200);
    GPSSerial.begin(9600, SERIAL_8N1, 15, 4);
    setupGPSFastLock();
    SerialBT.begin("ESP32_JRM");

    Serial.println("\n----------------------------------------");
    Serial.println("ESP32 GPS Speed Controller Started");
    Serial.println("Bluetooth SSID: ESP32_JRM");
    Serial.println("----------------------------------------");

    pinMode(pinRedLED, OUTPUT);
    pinMode(pinYellowLED, OUTPUT);
    pinMode(pinGreenLED, OUTPUT);
    pinMode(pinBuzzer, OUTPUT);
    pinMode(pinButton, INPUT_PULLUP);

    digitalWrite(pinRedLED, LOW);
    digitalWrite(pinYellowLED, LOW);
    digitalWrite(pinGreenLED, LOW);
    digitalWrite(pinBuzzer, LOW);

    SPI.begin(18, 19, 23, 5);
    if (!SD.begin(5)) {
        Serial.println("[SD] Initialization FAILED! Please verify connections/SD card.");
        SerialBT.println("[SD] Initialization FAILED! Please verify connections/SD card.");
    } else {
        Serial.println("[SD] Initialized successfully.");
        SerialBT.println("[SD] Initialized successfully.");
    }

    // Restore saved route & language from memory
    prefs.begin("config", false);
    String savedRoute = prefs.getString("route", "");
    if (savedRoute != "") {
        String filename = "/Route" + savedRoute + ".txt";
        if (SD.exists(filename) && loadRouteFile(filename)) {
            currentLang = (Language)prefs.getInt("lang", 0);
            currentRouteName = "Route" + savedRoute;
            currentState = STATE_WAITING_GPS;
            gpsLockStartTime = millis();
            String langStr = (currentLang == LANG_ENGLISH) ? "ENGLISH" : ((currentLang == LANG_HINDI) ? "HINDI" : "MARATHI");
            Serial.printf("[RESTORE] Resuming saved Route: %s | Language: %s\n", currentRouteName.c_str(), langStr.c_str());
            SerialBT.printf("[RESTORE] Resuming saved Route: %s | Language: %s\n", currentRouteName.c_str(), langStr.c_str());
        }
    }

    setupAudio();
}

void loop() {
    updateAudio();
    updateBuzzer();
    checkSerialInput();

    bool btnPressed = checkButtonPress();

    // Global on-the-fly language change (if not in initial setup state)
    if (btnPressed && currentState != STATE_LANG_SELECT) {
        int langInt = (int)currentLang;
        langInt++;
        if (langInt > 2) langInt = 0;
        currentLang = (Language)langInt;
        
        String finalLang = (currentLang == LANG_ENGLISH) ? "ENGLISH" : ((currentLang == LANG_HINDI) ? "HINDI" : "MARATHI");
        prefs.putInt("lang", (int)currentLang); // Save immediately
        
        Serial.printf("[LANG] Language changed on-the-fly to: %s\n", finalLang.c_str());
        SerialBT.printf("[LANG] Language changed on-the-fly to: %s\n", finalLang.c_str());
        
        triggerBuzzer(1, 200, 0); // Short beep to confirm
        
        // Re-play current zone audio in the new language if driving
        if (currentState == STATE_RUNNING && currentZone != ' ') {
            const char* audioPath = getZoneAudioPath(currentZone, currentLang);
            if (strlen(audioPath) > 0) playAudio(audioPath);
        }
    }

    switch (currentState) {

        // ---------------- STATE 1: LANGUAGE SELECTION ----------------
        case STATE_LANG_SELECT: {
            if (btnPressed) {
                buttonPressCount++;
                if (buttonPressCount > 3) buttonPressCount = 1;
                lastButtonPressTime = millis();
                String langName = (buttonPressCount == 1) ? "ENGLISH" : ((buttonPressCount == 2) ? "HINDI" : "MARATHI");
                Serial.printf("[LANG] Selection: %s (Confirming in 3 seconds...)\n", langName.c_str());
                SerialBT.printf("[LANG] Selection: %s (Confirming in 3 seconds...)\n", langName.c_str());
            }
            if (buttonPressCount > 0 && (millis() - lastButtonPressTime >= 3000)) {
                if      (buttonPressCount == 1) currentLang = LANG_ENGLISH;
                else if (buttonPressCount == 2) currentLang = LANG_HINDI;
                else if (buttonPressCount == 3) currentLang = LANG_MARATHI;
                String finalLang = (currentLang == LANG_ENGLISH) ? "ENGLISH" : ((currentLang == LANG_HINDI) ? "HINDI" : "MARATHI");
                prefs.putInt("lang", (int)currentLang); // Save language to memory
                Serial.printf("[LANG] Confirmed Language: %s\n", finalLang.c_str());
                SerialBT.printf("[LANG] Confirmed Language: %s\n", finalLang.c_str());
                triggerBuzzer(1, 300, 0);
                updateLEDs(' ');
                currentState = STATE_ROUTE_SELECT;
                Serial.println("[STATE] Transitioned to STATE_ROUTE_SELECT. Awaiting Route Selection...");
                SerialBT.println("[STATE] Transitioned to STATE_ROUTE_SELECT. Awaiting Route Selection...");
            }
            break;
        }

        // ---------------- STATE 2: ROUTE SELECTION ----------------
        case STATE_ROUTE_SELECT: {
            if (millis() - lastStatusPrintTime >= 10000) {
                lastStatusPrintTime = millis();
                Serial.println("[ROUTE] Please send route command (e.g. Route500, Route510, Route520) via Serial or Bluetooth.");
                SerialBT.println("[ROUTE] Please send route command (e.g. Route500, Route510, Route520) via Serial or Bluetooth.");
            }
            break;
        }

        // ---------------- STATE 3: GPS LOCK WAIT ----------------
        case STATE_WAITING_GPS: {
            
            // Keep all LEDs OFF until GPS lock
               digitalWrite(pinGreenLED, LOW);
               digitalWrite(pinYellowLED, LOW);
               digitalWrite(pinRedLED, LOW);

            while (GPSSerial.available()) gps.encode(GPSSerial.read());

            if (gps.location.isValid() && gps.location.age() < 2000) {
                double actLat = gps.location.lat();
                double actLon = gps.location.lng();
                unsigned long elapsed = millis() - gpsLockStartTime;
                Serial.printf("[GPS] Lock acquired in %.1f s! Lat: %.6f, Lon: %.6f | Sats: %d\n",
                    elapsed/1000.0, actLat, actLon,
                    gps.satellites.isValid() ? (int)gps.satellites.value() : 0);
                SerialBT.printf("[GPS] Lock acquired in %.1f s! Lat: %.6f, Lon: %.6f | Sats: %d\n",
                    elapsed/1000.0, actLat, actLon,
                    gps.satellites.isValid() ? (int)gps.satellites.value() : 0);

                char detectedZone = 'G';
                if (routePointCount > 0) {
                    double minDist = 999999.0; int closestIdx = -1;
                    for (int i = 0; i < routePointCount; i++) {
                        double dist = calculateDistance(actLat, actLon, routePoints[i].lat, routePoints[i].lon);
                        if (dist < minDist) { minDist = dist; closestIdx = i; }
                    }
                    if (closestIdx != -1 && minDist <= 150.0) {
                        detectedZone = routePoints[closestIdx].zone;
                        Serial.printf("[ZONE] Initial match: Zone '%c' (%.1f m from route point)\n", detectedZone, minDist);
                        SerialBT.printf("[ZONE] Initial match: Zone '%c' (%.1f m from route point)\n", detectedZone, minDist);
                    } else {
                        Serial.printf("[ZONE] Off-route (%.1f m). Defaulting to GREEN zone.\n", minDist);
                        SerialBT.printf("[ZONE] Off-route (%.1f m). Defaulting to GREEN zone.\n", minDist);
                    }
                }

                currentZone = detectedZone;
                updateLEDs(currentZone);
                const char* audioPath = getZoneAudioPath(currentZone, currentLang);
                if (strlen(audioPath) > 0) playAudio(audioPath);
                String zName = (currentZone == 'G') ? "GREEN" : ((currentZone == 'Y') ? "YELLOW" : "RED");
                Serial.printf("[STATE] Initial Zone: %s | Transitioning to RUNNING.\n", zName.c_str());
                SerialBT.printf("[STATE] Initial Zone: %s | Transitioning to RUNNING.\n", zName.c_str());
                triggerBuzzer(2, 200, 100); // Beep twice on success
                currentState = STATE_RUNNING;

            } else {
                if (millis() - lastStatusPrintTime >= 10000) {
                    lastStatusPrintTime = millis();
                    unsigned long waited = (millis() - gpsLockStartTime) / 1000;
                    Serial.printf("[GPS] Waiting for lock... %lu s elapsed | Sats: %d | HDOP: %.1f\n",
                        waited,
                        gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
                        gps.hdop.isValid() ? gps.hdop.hdop() : 99.9);
                    SerialBT.printf("[GPS] Waiting for lock... %lu s elapsed | Sats: %d | HDOP: %.1f\n",
                        waited,
                        gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
                        gps.hdop.isValid() ? gps.hdop.hdop() : 99.9);
                }
            }
            break;
        }

        // ---------------- STATE 4: RUNNING MONITORING LOOP ----------------
        case STATE_RUNNING: {
            while (GPSSerial.available()) gps.encode(GPSSerial.read());

            double actLat = 0.0, actLon = 0.0;
            bool locationAvailable = false;
            if (isLocationSimulated) {
                actLat = simulatedLat; actLon = simulatedLon; locationAvailable = true;
            } else if (gps.location.isValid() && gps.location.age() < 5000) {
                actLat = gps.location.lat(); actLon = gps.location.lng(); locationAvailable = true;
            }

            char detectedZone = ' ';
            if (isZoneManualOverridden) {
                detectedZone = manualOverriddenZone;
            } else if (locationAvailable && routePointCount > 0) {
                double minDistance = 999999.0; int closestIdx = -1;
                for (int i = 0; i < routePointCount; i++) {
                    double dist = calculateDistance(actLat, actLon, routePoints[i].lat, routePoints[i].lon);
                    if (dist < minDistance) { minDistance = dist; closestIdx = i; }
                }
                if (closestIdx != -1 && minDistance <= 150.0) {
                    detectedZone = routePoints[closestIdx].zone;
                } else {
                    // Off-route: hold the current zone if already in one, otherwise stay silent.
                    detectedZone = (currentZone != ' ') ? currentZone : ' ';
                    if (millis() - lastStatusPrintTime >= 10000) {
                        Serial.printf("[WARN] Off route! Closest pt distance: %.1f meters.\n", minDistance);
                        SerialBT.printf("[WARN] Off route! Closest pt distance: %.1f meters.\n", minDistance);
                    }
                }
            } else {
                detectedZone = (currentZone != ' ') ? currentZone : ' ';
            }

            if (detectedZone != currentZone) {
                currentZone = detectedZone;
                updateLEDs(currentZone);
                const char* audioPath = getZoneAudioPath(currentZone, currentLang);
                if (strlen(audioPath) > 0) playAudio(audioPath);
                String zName = (currentZone == 'G') ? "GREEN" : ((currentZone == 'Y') ? "YELLOW" : "RED");
                Serial.printf("[ZONE] Entry: %s ZONE\n", zName.c_str());
                SerialBT.printf("[ZONE] Entry: %s ZONE\n", zName.c_str());
            }

            double currentSpeedKmh = 0.0;
            if (isSpeedSimulated) {
                currentSpeedKmh = simulatedSpeedKmh;
            } else if (gps.speed.isValid() && gps.speed.age() < 5000) {
                currentSpeedKmh = gps.speed.knots() * 1.852;
            }

            double speedLimit = 65.0;
            if (currentZone == 'Y') speedLimit = 40.0;
            else if (currentZone == 'R') speedLimit = 30.0;

            if (currentZone != ' ' && currentSpeedKmh > speedLimit) {
                if (!isOverspeeding) {
                    isOverspeeding = true;
                    triggerBuzzer(3, 150, 150);
                    const char* ovsAudio = getOverspeedAudioPath(currentLang);
                    if (strlen(ovsAudio) > 0) playAudio(ovsAudio);
                    Serial.printf("[ALERT] OVERSPEED DETECTED! Speed: %.2f km/h, Limit: %.2f km/h\n", currentSpeedKmh, speedLimit);
                    SerialBT.printf("[ALERT] OVERSPEED DETECTED! Speed: %.2f km/h, Limit: %.2f km/h\n", currentSpeedKmh, speedLimit);
                }
            } else {
                if (isOverspeeding) {
                    isOverspeeding = false;
                    Serial.printf("[SPEED] Normalized: %.2f km/h\n", currentSpeedKmh);
                    SerialBT.printf("[SPEED] Normalized: %.2f km/h\n", currentSpeedKmh);
                }
            }

            if (millis() - lastStatusPrintTime >= 5000) {
                lastStatusPrintTime = millis();
                String zName = (currentZone == 'G') ? "GREEN" : ((currentZone == 'Y') ? "YELLOW" : ((currentZone == 'R') ? "RED" : "NONE"));
                String gpsState = isLocationSimulated ? "SIMULATED" : (gps.location.isValid() ? "FIX OK" : "NO FIX");
                String langStr = (currentLang == LANG_ENGLISH) ? "EN" : ((currentLang == LANG_HINDI) ? "HI" : "MR");
                Serial.printf("[STATUS] GPS: %s | Lat: %.6f, Lon: %.6f | Speed: %.1f km/h | Zone: %s (Limit: %.0f) | Route: %s | Lang: %s | Overspeed: %s\n",
                    gpsState.c_str(), actLat, actLon, currentSpeedKmh, zName.c_str(), speedLimit, currentRouteName.c_str(), langStr.c_str(), isOverspeeding ? "YES" : "NO");
                SerialBT.printf("[STATUS] GPS: %s | Lat: %.6f, Lon: %.6f | Speed: %.1f km/h | Zone: %s (Limit: %.0f) | Route: %s | Lang: %s | Overspeed: %s\n",
                    gpsState.c_str(), actLat, actLon, currentSpeedKmh, zName.c_str(), speedLimit, currentRouteName.c_str(), langStr.c_str(), isOverspeeding ? "YES" : "NO");
            }

            break;
        }
    }
}