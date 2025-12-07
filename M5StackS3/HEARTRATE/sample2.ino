#include <M5CoreS3.h>
#include <Wire.h>
#include <math.h>
#include "MAX30100_PulseOximeter.h"
#include <Adafruit_NeoPixel.h>

#define PIN        18 // PortB 🐱
#define NUMPIXELS 70
Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);

#define SAMPLING_RATE   (MAX30100_SAMPRATE_100HZ)
#define IR_LED_CURRENT  (MAX30100_LED_CURR_24MA)
#define RED_LED_CURRENT (MAX30100_LED_CURR_27_1MA)
#define PULSE_WIDTH     (MAX30100_SPC_PW_1600US_16BITS)
#define HIGHRES_MODE    (true)
#define REPORTING_PERIOD_MS 1000
#define HEALTH_CHECK_MS     500  // 1秒ごとにヘルスチェック 🔍

#define MAX30100_I2C_ADDRESS 0x57  // MAX30100のI2Cアドレス 📡

PulseOximeter pox;
MAX30100 sensor;
uint32_t tsLastReport = 0;
uint32_t tsLastHealthCheck = 0;
bool beatflg = false;
int beatCount = 0;
int reinitCount = 0;

void onBeatDetected()
{
    beatCount++;
    Serial.print("💓 Beat #");
    Serial.println(beatCount);
    
    if (beatflg) {
        M5.Lcd.fillCircle(30, 40, 10, BLACK);
        M5.Lcd.fillCircle(50, 40, 10, BLACK);
        M5.Lcd.fillCircle(40, 41, 3, BLACK);
        M5.Lcd.fillTriangle(22, 45, 58, 45, 40, 65, BLACK);
        pixels.clear();
        //pixels.setBrightness(1);
        for (int i = 1; i < NUMPIXELS; i++) {
            // delay(10);
            int r = random(255);
            int g = random(155);
            int b = random(155);
            pixels.setPixelColor(i, pixels.Color(r, g, b));
        }
        pixels.show();
        beatflg = false;
    }
    else {
        M5.Lcd.fillCircle(30, 40, 10, RED);
        M5.Lcd.fillCircle(50, 40, 10, RED);
        M5.Lcd.fillCircle(40, 41, 3, RED);
        M5.Lcd.fillTriangle(22, 45, 58, 45, 40, 65, RED);
        //pixels.setBrightness(10);
        pixels.clear();
        for (int i = 1; i < NUMPIXELS; i++) {
            // delay(10);
            pixels.setPixelColor(i, pixels.Color(1,1,1));
        }
        pixels.show();
        beatflg = true;
    }
}

// センサーが生きているか確認 🔍💓
bool isSensorAlive() {
    Wire.beginTransmission(MAX30100_I2C_ADDRESS);
    byte error = Wire.endTransmission();
    
    if (error == 0) {
        // I2C応答あり - さらにレジスタを読んで確認
        Wire.beginTransmission(MAX30100_I2C_ADDRESS);
        Wire.write(0xFF);  // Part ID レジスタ
        Wire.endTransmission(false);
        Wire.requestFrom(MAX30100_I2C_ADDRESS, (uint8_t)1);
        
        if (Wire.available()) {
            byte partId = Wire.read();
            // MAX30100のPart IDは0x11
            if (partId == 0x11) {
                return true;  // センサーは生きている ✅
            }
        }
    }
    
    return false;  // センサーが応答しない ❌
}

// LEDが光っているか確認 (モードレジスタをチェック) 💡
bool isLedActive() {
    Wire.beginTransmission(MAX30100_I2C_ADDRESS);
    Wire.write(0x06);  // Mode Configuration レジスタ
    Wire.endTransmission(false);
    Wire.requestFrom(MAX30100_I2C_ADDRESS, (uint8_t)1);
    
    if (Wire.available()) {
        byte mode = Wire.read();
        // SpO2モード (0x03) または HR モード (0x02) がアクティブか確認
        byte modeValue = mode & 0x07;
        return (modeValue == 0x02 || modeValue == 0x03);
    }
    
    return false;
}

// センサーを再初期化 🔄
void reinitSensor() {
    reinitCount++;
    Serial.print("🔄 Re-initializing sensor... (#");
    Serial.print(reinitCount);
    Serial.println(")");
    
    // センサー設定
    sensor.setMode(MAX30100_MODE_SPO2_HR);
    sensor.setLedsCurrent(IR_LED_CURRENT, RED_LED_CURRENT);
    sensor.setLedsPulseWidth(PULSE_WIDTH);
    sensor.setSamplingRate(SAMPLING_RATE);
    sensor.setHighresModeEnabled(HIGHRES_MODE);
    
    Serial.println("✅ Sensor re-initialized");
    
    // LCD表示更新
    M5.Lcd.fillRect(100, 140, 220, 10, BLACK);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(0, 140);
    M5.Lcd.setTextColor(ORANGE);
    M5.Lcd.print("Reinit: ");
    M5.Lcd.print(reinitCount);
    M5.Lcd.setTextColor(WHITE);
}

void setup()
{
    M5.begin();
    M5.Power.begin();
    Serial.begin(115200);
    
    Serial.println("\n🚀 MAX30100 + NECO (Smart Health Check)");
    Serial.println("========================================");

    randomSeed(analogRead(0));
    
    // I2C初期化 📍
    Serial.println("📍 I2C PortA (SDA=2, SCL=1)");
    //Wire.begin(2, 1);
    // delay(500);

    
    // 猫耳LED 🐱
    Serial.println("🐱 NECO Unit...");
    pixels.setBrightness(10);
    pixels.begin();
    pixels.clear();
    pixels.show();
    Serial.println("✅ NECO OK");
    
    Serial.println("🏥 Initializing MAX30100...");
    
    // UI ❤️
    M5.Lcd.fillCircle(30, 40, 10, RED);
    M5.Lcd.fillCircle(50, 40, 10, RED);
    M5.Lcd.fillCircle(40, 41, 3, RED);
    M5.Lcd.fillTriangle(22, 45, 58, 45, 40, 65, RED);

    M5.Lcd.setTextSize(3);
    M5.Lcd.setCursor(20, 80);
    M5.Lcd.print("O");
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(48, 100);
    M5.Lcd.print("2");

    // センサー初期化 🏥
    while (!sensor.begin()) {
        Serial.println("  Sensor not found...");
        delay(1000);
    }
    Serial.println("✅ Sensor found!");
    
    // Part IDを確認 📊
    Serial.print("  Part ID: ");
    if (isSensorAlive()) {
        Serial.println("0x11 (MAX30100) ✅");
    } else {
        Serial.println("Unknown ⚠️");
    }
    
    // センサー設定 ⚙️
    sensor.setMode(MAX30100_MODE_SPO2_HR);
    sensor.setLedsCurrent(IR_LED_CURRENT, RED_LED_CURRENT);
    sensor.setLedsPulseWidth(PULSE_WIDTH);
    sensor.setSamplingRate(SAMPLING_RATE);
    sensor.setHighresModeEnabled(HIGHRES_MODE);
    
    pox.setOnBeatDetectedCallback(onBeatDetected);
    
    Serial.println("\n💡 Smart health check enabled");
    Serial.println("🔍 Sensor will be reinit if dead");
    Serial.println("👆 Place finger on sensor");
    Serial.println("========================================\n");
    
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(0, 120);
    M5.Lcd.setTextColor(CYAN);
    M5.Lcd.print("Smart Health Check");
    M5.Lcd.setCursor(0, 130);
    M5.Lcd.setTextColor(GREEN);
    M5.Lcd.print("Beats: 0");
    M5.Lcd.setTextColor(WHITE);
    
    tsLastHealthCheck = millis();
}

void loop()
{
    // pox.update() 💓
    pox.update();
    
    // ヘルスチェック 🔍
    if (millis() - tsLastHealthCheck > HEALTH_CHECK_MS) {
        bool alive = isSensorAlive();
        bool ledOn = isLedActive();
        
        if (!alive) {
            Serial.println("❌ Sensor not responding on I2C!");
            // I2Cを再初期化
            Wire.end();
            // delay(100);
            //Wire.begin(2, 1);
            //delay(100);
            
            if (sensor.begin()) {
                reinitSensor();
            }
        } else if (!ledOn) {
            Serial.println("⚠️ Sensor alive but LED mode inactive!");
            reinitSensor();
        } else {
            // センサーは正常 ✅
            // Serial.println("✅ Sensor healthy");  // デバッグ用
        }
        
        tsLastHealthCheck = millis();
    }

    // レポート 📟
    if (millis() - tsLastReport > REPORTING_PERIOD_MS) {
        float hr = pox.getHeartRate();
        float spo2 = pox.getSpO2();
        bool alive = isSensorAlive();
        bool ledOn = isLedActive();
        
        Serial.print("💓 HR: ");
        Serial.print(hr, 1);
        Serial.print(" | 🫁 SpO2: ");
        Serial.print(spo2, 1);
        Serial.print(" | Beats: ");
        Serial.print(beatCount);
        Serial.print(" | Alive: ");
        Serial.print(alive ? "✅" : "❌");
        Serial.print(" | LED: ");
        Serial.println(ledOn ? "✅" : "❌");
        
        M5.Lcd.setTextSize(3);
        
        // 心拍数 💗
        M5.Lcd.fillRect(75, 40, 150, 25, BLACK);
        M5.Lcd.setCursor(75, 40);
        if (hr > 0 && hr < 200) {
            M5.Lcd.setTextColor(GREEN);
            M5.Lcd.print(hr, 0);
        } else {
            M5.Lcd.setTextColor(YELLOW);
            M5.Lcd.print("--");
        }
        M5.Lcd.setTextColor(WHITE);

        // SpO2 🫁
        M5.Lcd.fillRect(75, 90, 150, 25, BLACK);
        M5.Lcd.setCursor(75, 90);
        if (spo2 > 0 && spo2 <= 100) {
            M5.Lcd.setTextColor(GREEN);
            M5.Lcd.print(spo2, 0);
        } else {
            M5.Lcd.setTextColor(YELLOW);
            M5.Lcd.print("--");
        }
        M5.Lcd.setTextColor(WHITE);
        
        // ステータス表示
        M5.Lcd.fillRect(50, 130, 270, 10, BLACK);
        M5.Lcd.setTextSize(1);
        M5.Lcd.setCursor(0, 130);
        M5.Lcd.print("Beats:");
        M5.Lcd.print(beatCount);
        M5.Lcd.print(" Alive:");
        M5.Lcd.print(alive ? "Y" : "N");
        M5.Lcd.print(" LED:");
        M5.Lcd.print(ledOn ? "Y" : "N");

        tsLastReport = millis();
    }
}
