/*
 * 使用ユニット / Units Used:
 *   - MAX30100 心拍センサー / Heart Rate Sensor (I2C)
 *   - NECO Unit (NeoPixel LED)
 * 
 * ピン配置 / Pin Assignment:
 *   - MAX30100: PortA I2C (SDA=2, SCL=1)
 *   - NECO Unit: PortC (GPIO 17)
 */

#include <M5CoreS3.h>
#include <Wire.h>
#include <math.h>
#include "MAX30100_PulseOximeter.h"
#include <Adafruit_NeoPixel.h>

#define PIN        17 // PortC 🐱
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

static const int FACE_CENTER_X = 255;
static const int FACE_CENTER_Y = 110;

PulseOximeter pox;
MAX30100 sensor;
uint32_t tsLastReport = 0;
uint32_t tsLastHealthCheck = 0;
bool beatflg = false;
int beatCount = 0;
int reinitCount = 0;
float lastHeartRate = 0.0f;

#define PIXEL_UPDATE_MS 1000

void drawFace(float hr, bool blink)
{
    const int faceRadius = 55;

    bool isHigh = hr > 110;
    bool isLow = (hr > 0 && hr < 65);

    int swing = 0;
    if (beatCount > 0 && hr > 0) {
        swing = (int)(hr * 0.06f); // HRに応じて振れ幅（控えめ）
        swing = max(0, min(6, swing));
    }
    int xOffset = (beatCount > 0) ? ((beatCount & 1) ? swing : -swing) : 0;
    int centerX = FACE_CENTER_X + xOffset;
    int centerY = FACE_CENTER_Y;

    uint16_t bodyColor = M5.Lcd.color565(255, 236, 200);   // ベージュ
    uint16_t shellColor = M5.Lcd.color565(255, 150, 200);  // ピンクの甲羅
    uint16_t bellyColor = RED;                             // 赤いおなか
    uint16_t outlineColor = BLACK;
    uint16_t cheekColor = M5.Lcd.color565(255, 170, 190);
    uint16_t blushLow = M5.Lcd.color565(230, 160, 200);
    uint16_t blushHigh = M5.Lcd.color565(255, 120, 140);

    // クリア領域を固定し、他UIと重ならない右側だけを消去（スイング±6pxを包含）
    const int clearX = FACE_CENTER_X - 85;
    const int clearY = FACE_CENTER_Y - 90;
    const int clearW = 170;
    const int clearH = 185;
    M5.Lcd.fillRect(clearX, clearY, clearW, clearH, BLACK);

    // ピンクの甲羅（さらにコンパクトに）
    M5.Lcd.fillRoundRect(centerX - 74, centerY - 42, 62, 96, 26, shellColor);
    M5.Lcd.drawRoundRect(centerX - 74, centerY - 42, 62, 96, 26, outlineColor);
    // 下側をふっくら見せる追い円（塗りのみで輪郭線は重ねない）
    M5.Lcd.fillCircle(centerX - 54, centerY + 28, 20, shellColor);

    // 体（ベージュの胴体）
    M5.Lcd.fillRoundRect(centerX - 46, centerY - 54, 92, 118, 38, bodyColor);
    M5.Lcd.drawRoundRect(centerX - 46, centerY - 54, 92, 118, 38, outlineColor);

    // 赤いおなか（パンツではなく大きなお腹）
    M5.Lcd.fillRoundRect(centerX - 44, centerY + 6, 88, 70, 30, bellyColor);
    M5.Lcd.drawRoundRect(centerX - 44, centerY + 6, 88, 70, 30, outlineColor);

    // 足
    M5.Lcd.fillCircle(centerX - 24, centerY + 72, 12, bodyColor);
    M5.Lcd.drawCircle(centerX - 24, centerY + 72, 12, outlineColor);
    M5.Lcd.fillCircle(centerX + 24, centerY + 72, 12, bodyColor);
    M5.Lcd.drawCircle(centerX + 24, centerY + 72, 12, outlineColor);

    // 腕
    M5.Lcd.fillCircle(centerX - 50, centerY + 10, 12, bodyColor);
    M5.Lcd.drawCircle(centerX - 50, centerY + 10, 12, outlineColor);
    M5.Lcd.fillCircle(centerX + 50, centerY + 10, 12, bodyColor);
    M5.Lcd.drawCircle(centerX + 50, centerY + 10, 12, outlineColor);

    // 目（常に大きな目、瞬きしない）
    const uint16_t eyeColor = WHITE;
    const uint16_t pupilColor = outlineColor;
    int eyeR = 18; // 大きくはっきり
    int pupilR = isHigh ? 7 : 6; // HR高は少し大きく
    int pupilOffsetY = isHigh ? -1 : 0; // 低でも細目にしない
    int eyeLX = centerX - 12;
    int eyeRX = centerX + 22;
    int eyeY  = centerY - 26;
    M5.Lcd.fillCircle(eyeLX, eyeY, eyeR, eyeColor);
    M5.Lcd.drawCircle(eyeLX, eyeY, eyeR, outlineColor);
    M5.Lcd.fillCircle(eyeRX, eyeY, eyeR, eyeColor);
    M5.Lcd.drawCircle(eyeRX, eyeY, eyeR, outlineColor);
    M5.Lcd.fillCircle(eyeLX + 5, eyeY - 2 + pupilOffsetY, pupilR, pupilColor);
    M5.Lcd.fillCircle(eyeRX + 5, eyeY - 2 + pupilOffsetY, pupilR, pupilColor);

    // ほっぺ
    uint16_t cheekTone = isHigh ? blushHigh : (isLow ? blushLow : cheekColor);
    M5.Lcd.fillCircle(centerX - 32, centerY + 6, 10, cheekTone); // 左頬を強めに
    M5.Lcd.fillCircle(centerX + 30, centerY + 6, 6, blushLow);  // 右は淡く小さく

    // 口
    int mouthY = centerY + 22;
    if (isHigh) {
        M5.Lcd.fillRoundRect(centerX - 6, mouthY, 12, 6, 3, RED);
    } else if (isLow) {
        M5.Lcd.fillRoundRect(centerX - 7, mouthY + 4, 14, 5, 2, RED);
    } else {
        M5.Lcd.fillRoundRect(centerX - 6, mouthY + 2, 12, 6, 3, RED);
    }

    // シンプルな鼻（目の間に小さく）
    M5.Lcd.fillCircle(centerX, centerY - 6, 2, outlineColor);
}

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

    drawFace(lastHeartRate, beatflg);
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

    drawFace(0, false);

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
        M5.Lcd.fillRect(0, 130, 170, 10, BLACK);
        M5.Lcd.setTextSize(1);
        M5.Lcd.setCursor(0, 130);
        M5.Lcd.print("Beats:");
        M5.Lcd.print(beatCount);
        M5.Lcd.print(" Alive:");
        M5.Lcd.print(alive ? "Y" : "N");
        M5.Lcd.print(" LED:");
        M5.Lcd.print(ledOn ? "Y" : "N");

        lastHeartRate = hr;

        tsLastReport = millis();
    }
}
