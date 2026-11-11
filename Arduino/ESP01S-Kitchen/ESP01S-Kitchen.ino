#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <Ticker.h>
#include <WiFiUdp.h>

#include <WebSocketsServer.h>
#if __has_include("credentials.h")
#include "credentials.h"
#endif

#ifndef CREDENTIALS_H
#define WIFI_SSID "default_ssid"
#define WIFI_PASS "default_pass"
#define OTA_PASSWORD "default_ota_pass"
#endif

volatile bool timer_finished_event = false;

// --- GPIOs ---
WebSocketsServer webSocket = WebSocketsServer(81, "/ws");
const int SWITCH_PIN = 2;  // Button
const int OUTPUT_PIN = 0;  // Heater SSR
// const int ACTIVE_PIN = 3;  // RX pin -> Indicator LED 1
const int ACTIVE2_PIN = 1; // TX pin -> Indicator LED 2

// --- Globals ---
ESP8266WebServer server(80);

Ticker countdownTimer;

volatile bool system_on = false;

// WebSocket event handler (we don't expect incoming messages now)
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    if (type == WStype_CONNECTED) {
        // client connected
    } else if (type == WStype_TEXT) {
        // handle incoming text if needed
    }
}

volatile bool output_state = false;
volatile int initial_on_time = 2;   // minutes
volatile int pwm_period = 30;        // seconds
volatile int pwm_duty = 100;        // percent
volatile int timer_minutes = 0;     // minutes
volatile int time_left_seconds = 0; // seconds

// State Machine variables
unsigned long stateChangeMillis = 0;
bool initialPeriodDone = false;
unsigned long lastDebounceTime = 0;
int lastButtonState = HIGH;

// --- State Helpers ---
String getStatusString() {
    if (!system_on) return "Off";
    if (timer_minutes > 0 && pwm_period > 0) return "On - Temporizador & Regulado";
    if (timer_minutes > 0) return "On - Temporizador";
    if (pwm_period > 0) return "On - Regulado";
    return "On";
}

void timerTick() {
    if (system_on && timer_minutes > 0 && time_left_seconds > 0) {
        time_left_seconds--;
        if (time_left_seconds <= 0) {
            system_on = false;
            timer_minutes = 0;
            timer_finished_event = true;
        }
    }
}

// --- Non-Blocking Heater State Machine ---
void handleHeaterLogic() {
    static bool was_on = false;

    // Reset initial run flag if transitioning OFF -> ON
    if (system_on && !was_on) {
        initialPeriodDone = false;
        stateChangeMillis = millis();
    }
    was_on = system_on;

    if (!system_on) {
        output_state = false;
        digitalWrite(OUTPUT_PIN, LOW);
        // Indicator LEDs turn OFF when system is OFF
        digitalWrite(ACTIVE2_PIN, HIGH);
        return;
    }

    // System is ON: Indicator LEDs turn ON and stay ON regardless of PWM phase
    digitalWrite(ACTIVE2_PIN, LOW);

    unsigned long currentMillis = millis();
    unsigned long elapsed = currentMillis - stateChangeMillis;

    // Priority 1: Initial ON Time
    if (initial_on_time > 0 && !initialPeriodDone) {
        output_state = true;
        digitalWrite(OUTPUT_PIN, HIGH);
        
        if (elapsed >= (initial_on_time * 60000UL)) {
            initialPeriodDone = true;
            stateChangeMillis = currentMillis; // Reset timer for PWM phase
        }
        return; 
    }

    // Priority 2: PWM Logic
    if (pwm_period > 0) {
        if (pwm_duty >= 100) {
            output_state = true;
            digitalWrite(OUTPUT_PIN, HIGH);
        } else if (pwm_duty <= 0) {
            output_state = false;
            digitalWrite(OUTPUT_PIN, LOW);
        } else {
            unsigned long period_ms = pwm_period * 1000UL;
            unsigned long on_time_ms = (period_ms * pwm_duty) / 100;
            
            // Where are we in the current PWM cycle?
            unsigned long cycle_pos = elapsed % period_ms;
            
            if (cycle_pos < on_time_ms) {
                output_state = true;
                digitalWrite(OUTPUT_PIN, HIGH);
            } else {
                output_state = false;
                digitalWrite(OUTPUT_PIN, LOW);
            }
        }
    } else {
        // No PWM, steady ON
        output_state = true;
        digitalWrite(OUTPUT_PIN, HIGH);
    }
}

// --- Web Handlers ---
void handleState() {
    String json = "{";
    json += "\"status\":\"" + getStatusString() + "\",";
    json += "\"output_state\":\"" + String(output_state ? "ON" : "OFF") + "\",";
    json += "\"time_left\":\"" + String(time_left_seconds / 60) + " min " + String(time_left_seconds % 60) + " sec\",";
    json += "\"initial_on_time\":" + String(initial_on_time) + ",";
    json += "\"pwm_period\":" + String(pwm_period) + ",";
    json += "\"pwm_duty\":" + String(pwm_duty) + ",";
    json += "\"timer_minutes\":" + String(timer_minutes);
    json += "}";
    server.send(200, "application/json", json);
}

void handlePost() {
    if (server.hasArg("toggle") && server.arg("toggle") == "1") {
        system_on = !system_on;
        if (!system_on) { 
            timer_minutes = 0; 
            time_left_seconds = 0; 
        } else {
            if(timer_minutes > 0) time_left_seconds = timer_minutes * 60;
        }
    }
    if (server.hasArg("initial_on_time")) initial_on_time = server.arg("initial_on_time").toInt();
    if (server.hasArg("pwm_period")) pwm_period = server.arg("pwm_period").toInt();
    if (server.hasArg("pwm_duty")) pwm_duty = server.arg("pwm_duty").toInt();
    if (server.hasArg("timer_minutes")) {
        timer_minutes = server.arg("timer_minutes").toInt();
        if(system_on && timer_minutes > 0) time_left_seconds = timer_minutes * 60;
        if(timer_minutes == 0) time_left_seconds = 0;
    }
    
    server.sendHeader("Location", "/");
    server.send(303);
}

// --- OTA Web Handler ---
void handleUpdate() {
    HTTPUpload& upload = server.upload();
    
    if (upload.status == UPLOAD_FILE_START) {
        if (server.arg("password") != OTA_PASSWORD) {
            Update.end(false); // Abort if password is wrong
            return;
        }
        WiFiUDP::stopAll();
        
        // Check dropdown to see if we are updating Code or Filesystem
        int command = U_FLASH; // Default to Firmware
        size_t updateSize = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        
        if (server.arg("type") == "filesystem") {
            command = U_FS;
            updateSize = 0x10000; // 64KB for our 1M64 layout
        }
        
        Update.begin(updateSize, command);
    } 
    else if (upload.status == UPLOAD_FILE_WRITE) {
        if (server.arg("password") == OTA_PASSWORD) {
            Update.write(upload.buf, upload.currentSize);
        }
    } 
    else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            // Update successful
        }
    }
}

void setup() {
    // Note: Do NOT call Serial.begin() so GPIO1 and GPIO3 can be normal output pins
    
    pinMode(OUTPUT_PIN, OUTPUT);
    digitalWrite(OUTPUT_PIN, LOW);
    
    // Explicitly set TX/RX to normal GPIO Output mode
    // pinMode(ACTIVE_PIN, OUTPUT);
    // digitalWrite(ACTIVE_PIN, LOW);
    pinMode(ACTIVE2_PIN, OUTPUT);
    digitalWrite(ACTIVE2_PIN, LOW);

    pinMode(SWITCH_PIN, INPUT_PULLUP);

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    // Wait for WiFi
    while (WiFi.status() != WL_CONNECTED) {
        delay(250);
    }

    if (!LittleFS.begin()) {
        // Filesystem mount failed
    }
    
    server.on("/state", HTTP_GET, handleState);
    server.on("/post", HTTP_POST, handlePost);
    
    // OTA Route (multipart POST)
    server.on("/doUpdate", HTTP_POST, []() {
        server.sendHeader("Connection", "close");
        server.send(200, "text/plain", (Update.hasError() || server.arg("password") != OTA_PASSWORD) ? "FAIL" : "OK - RESTARTING");
        delay(1000);
        ESP.restart();
    }, handleUpdate);

    server.on("/", HTTP_GET, []() {
      File f = LittleFS.open("/index.html.gz", "r");
      if (!f) { server.send(404); return; }
      server.sendHeader("Content-Encoding", "gzip");
      server.sendHeader("Cache-Control", "max-age=3600");
      server.streamFile(f, "text/html");
      f.close();
    });

    server.serveStatic("/manifest.json", LittleFS, "/manifest.json", "max-age=86400");
    server.serveStatic("/icons", LittleFS, "/icons", "max-age=86400");

    server.begin();

    // Start WebSocket server
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    
    // Start standard Timer interrupt (1 second interval)
    countdownTimer.attach(1.0, timerTick);
}

void loop() {
    server.handleClient();
    webSocket.loop();
    
    // 1. Debounced Button Polling
    int reading = digitalRead(SWITCH_PIN);
    
    // If button is LOW (pressed), was previously HIGH (released), and 250ms have passed:
    if (reading == LOW && lastButtonState == HIGH && (millis() - lastDebounceTime > 250)) {
        lastDebounceTime = millis();
        system_on = !system_on;
        
        if (!system_on) {
            timer_minutes = 0;
            time_left_seconds = 0;
        } else if (timer_minutes > 0) {
            time_left_seconds = timer_minutes * 60;
        }
    }
    // Update the state for the next loop iteration
    lastButtonState = reading;

    // 2. Handle non-blocking heater and LED state
    handleHeaterLogic();
    
    // Small delay to prevent watchdog reset if logic gets stuck
    delay(10);

    if (timer_finished_event) {
        timer_finished_event = false;
        String msg = "{\"event\":\"timer_done\",\"message\":\"Timer finished\"}";
        webSocket.broadcastTXT(msg);
    }
}
