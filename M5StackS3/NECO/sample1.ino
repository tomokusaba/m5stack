// 🐱 猫耳LED点滅スケッチ (シンプル版)
// 心拍センサーなし - タイマーベースで点滅 ✨

#include <M5CoreS3.h>
#include <Wire.h>  // M5GFXが内部で使用するため必要
#include <Adafruit_NeoPixel.h>

#define PIN        2       // PortA 🐱
#define NUMPIXELS  70       // LED数
#define BLINK_INTERVAL_MS 1000  // 点滅間隔 (ミリ秒)

Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);

uint32_t lastBlinkTime = 0;
bool ledState = false;
int blinkCount = 0;

void setup()
{
    M5.begin();
    M5.Power.begin();
    Serial.begin(115200);
    
    Serial.println("\n🐱 NECO LED Blink (Simple Mode)");
    Serial.println("================================");

    randomSeed(analogRead(0));
    
    // 猫耳LED初期化 🐱
    Serial.println("🐱 Initializing NECO Unit...");
    pixels.setBrightness(10);
    pixels.begin();
    pixels.clear();
    pixels.show();
    Serial.println("✅ NECO OK!");
    
    // LCD表示 📺
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextSize(3);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(60, 100);
    M5.Lcd.print("NECO BLINK");
    
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(80, 150);
    M5.Lcd.setTextColor(CYAN);
    M5.Lcd.print("Simple Mode");
    
    Serial.println("\n✨ Starting blink loop...");
    Serial.println("================================\n");
}

void loop()
{
    // 点滅タイミングチェック ⏰
    if (millis() - lastBlinkTime >= BLINK_INTERVAL_MS) {
        lastBlinkTime = millis();
        blinkCount++;
        
        if (ledState) {
            // LEDオフ 🌑
            pixels.clear();
            pixels.show();
            ledState = false;
            Serial.println("🌑 OFF");
        }
        else {
            // LEDオン - ランダムカラー 🌈
            pixels.clear();
            for (int i = 0; i < NUMPIXELS; i++) {
                int r = random(100, 255);
                int g = random(50, 150);
                int b = random(50, 150);
                pixels.setPixelColor(i, pixels.Color(r, g, b));
            }
            pixels.show();
            ledState = true;
            Serial.print("🌈 ON - Blink #");
            Serial.println(blinkCount);
        }
        
        // LCDカウンター更新 📊
        M5.Lcd.fillRect(100, 200, 120, 30, BLACK);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setCursor(100, 200);
        M5.Lcd.setTextColor(GREEN);
        M5.Lcd.print("Count: ");
        M5.Lcd.print(blinkCount);
    }
}
