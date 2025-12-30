#include <M5CoreS3.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <Wire.h>
#include <math.h>
#include "MAX30100_PulseOximeter.h"
#include <Adafruit_NeoPixel.h>

// M5GFXのPNG描画はDataWrapper*を要求する構成があるため、
// SD.open()で得たfs::FileをDataWrapperに変換する薄いラッパーを用意する
#include <lgfx/v1/misc/DataWrapper.hpp>

#define PIN        18 // PortB 🐱
#define NUMPIXELS 70
Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);

#define SAMPLING_RATE   (MAX30100_SAMPRATE_100HZ)
#define IR_LED_CURRENT  (MAX30100_LED_CURR_24MA)
#define RED_LED_CURRENT (MAX30100_LED_CURR_27_1MA)
#define PULSE_WIDTH     (MAX30100_SPC_PW_1600US_16BITS)
#define HIGHRES_MODE    (true)
#define REPORTING_PERIOD_MS 1000

// CoreS3 microSD (SPI) pins
#define SD_SPI_CS_PIN   4
#define SD_SPI_SCK_PIN  36
#define SD_SPI_MISO_PIN 35
#define SD_SPI_MOSI_PIN 37
#define KAME_JPG_PATH   "/kame.jpg"

static const int FACE_CENTER_X = 255;
static const int FACE_CENTER_Y = 110;

// 右側キャラ（画像）領域との干渉回避用レイアウト
static const int CHAR_CLEAR_X = FACE_CENTER_X - 85; // drawFace()のclearXと合わせる
static const int TEXT_VALUE_X = 75;
static const int TEXT_RIGHT_MARGIN = 5;
static const int TEXT_VALUE_W = (CHAR_CLEAR_X - TEXT_RIGHT_MARGIN) - TEXT_VALUE_X;

PulseOximeter pox;
MAX30100 sensor;
uint32_t tsLastReport = 0;
bool beatflg = false;
int beatCount = 0;
float lastHeartRate = 0.0f;
bool sdReady = false;
volatile bool requestFaceRedraw = false;
bool kameSpriteReady = false;

// kame.jpgは起動時にSpriteへ展開して、ビート毎のSDアクセス/デコードを避ける
M5Canvas kameSprite(&M5.Lcd);

class FsFileDataWrapper final : public lgfx::v1::DataWrapper {
public:
    explicit FsFileDataWrapper(fs::File* file) : file_(file) {}

    int read(uint8_t* buf, uint32_t len) override {
        if (!file_ || !*file_) return 0;
        return (int)file_->read(buf, len);
    }

    void skip(int32_t offset) override {
        if (!file_ || !*file_) return;
        int32_t pos = tell();
        if (pos < 0) pos = 0;
        int32_t next = pos + offset;
        if (next < 0) next = 0;
        seek((uint32_t)next);
    }

    bool seek(uint32_t offset) override {
        if (!file_ || !*file_) return false;
        return file_->seek(offset);
    }

    void close(void) override {
        // 呼び出し側で明示的にcloseする（ここでは何もしない）
    }

    int32_t tell(void) override {
        if (!file_ || !*file_) return 0;
        return (int32_t)file_->position();
    }

private:
    fs::File* file_;
};

#define PIXEL_UPDATE_MS 1000

void drawFace(float hr, bool blink)
{
    // 心拍に応じて左右にスイング（見えるように少し大きめ）
    int swingPx = 0;
    if (hr > 0) {
        swingPx = (int)(hr * 0.12f);
        swingPx = max(4, min(16, swingPx));
    }
    int xOffset = (beatCount > 0) ? ((beatCount & 1) ? swingPx : -swingPx) : 0;

    // 右側キャラクター領域を消去して、SDのkame.jpgで置き換え
    // クリア領域を固定し、他UIと重ならない右側だけを消去（スイング±6pxを包含）
    const int clearX = FACE_CENTER_X - 85;
    const int clearY = FACE_CENTER_Y - 90;
    const int clearW = 170;
    const int clearH = 185;
    M5.Lcd.fillRect(clearX, clearY, clearW, clearH, BLACK);

    if (kameSpriteReady) {
        // 左に振れてもテキスト領域に被らないよう、基準Xを右へオフセット
        const int baseX = (beatCount > 0) ? (clearX + swingPx) : clearX;
        const int drawX = baseX + xOffset;
        const int drawY = clearY;
        kameSprite.pushSprite(drawX, drawY);
    }
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
        // pixels.setBrightness(10);
        // pixels.begin();
        pixels.clear();
        //pixels.setBrightness(1);
        for (int i = 1; i < NUMPIXELS; i++) {
            // delay(10);
            int r = random(155);
            int g = random(55);
            int b = random(55);
            int target = beatCount * 5 % NUMPIXELS;
            int diff = (i - target + NUMPIXELS) % NUMPIXELS;
            if (diff < 10) {
                r = 10;
                g = 10;
                b = 10;
            }

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

    // コールバック内で重い描画(SD/JPG等)をするとpox.update()が詰まってセンサーが止まりやすい。
    // ループ側で描画する。
    requestFaceRedraw = true;
}

void setup()
{
    M5.begin();
    M5.Power.begin();
    Serial.begin(115200);
    
    Serial.println("\n🚀 MAX30100 + NECO (Smart Health Check)");
    Serial.println("========================================");

    randomSeed(analogRead(0));

    // microSD初期化（公式PICサンプルに準拠）
    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
    sdReady = SD.begin(SD_SPI_CS_PIN, SPI, 25000000);
    if (sdReady) {
        Serial.println("✅ SD card detected");
        if (!SD.exists(KAME_JPG_PATH)) {
            Serial.println("⚠️ /kame.jpg not found on SD");
        }
    } else {
        Serial.println("⚠️ SD card not detected");
    }

    // kame.jpg をSpriteに読み込み（ビート毎のデコード回避）
    if (sdReady && SD.exists(KAME_JPG_PATH)) {
        const int spriteW = 170; // drawFace()のclearW
        const int spriteH = 185; // drawFace()のclearH
        kameSprite.setColorDepth(16);
        if (kameSprite.createSprite(spriteW, spriteH)) {
            kameSprite.fillSprite(BLACK);
            File file = SD.open(KAME_JPG_PATH, FILE_READ);
            if (file) {
                FsFileDataWrapper wrapper(&file);
                kameSprite.drawJpg(&wrapper, 0, 0);
                file.close();
                kameSpriteReady = true;
                Serial.println("✅ kame.jpg loaded into sprite");
            } else {
                Serial.println("⚠️ failed to open /kame.jpg");
            }
        } else {
            Serial.println("⚠️ failed to create sprite for kame.jpg");
        }
    }
    
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
    
    // センサー設定 ⚙️
    sensor.setMode(MAX30100_MODE_SPO2_HR);
    sensor.setLedsCurrent(IR_LED_CURRENT, RED_LED_CURRENT);
    sensor.setLedsPulseWidth(PULSE_WIDTH);
    sensor.setSamplingRate(SAMPLING_RATE);
    sensor.setHighresModeEnabled(HIGHRES_MODE);
    
    pox.setOnBeatDetectedCallback(onBeatDetected);
    
    Serial.println("\n� Place finger on sensor");
    Serial.println("========================================\n");
    
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(0, 130);
    M5.Lcd.setTextColor(GREEN);
    M5.Lcd.print("Beats: 0");
    M5.Lcd.setTextColor(WHITE);
}

void loop()
{
    // pox.update() 💓
    pox.update();

    // ビート検出後の描画（コールバック外で実行してセンサー更新頻度を確保）
    if (requestFaceRedraw) {
        requestFaceRedraw = false;
        drawFace(lastHeartRate, beatflg);
    }

    // レポート 📟
    if (millis() - tsLastReport > REPORTING_PERIOD_MS) {
        float hr = pox.getHeartRate();
        float spo2 = pox.getSpO2();
        
        Serial.print("💓 HR: ");
        Serial.print(hr, 1);
        Serial.print(" | 🫁 SpO2: ");
        Serial.print(spo2, 1);
        Serial.print(" | Beats: ");
        Serial.println(beatCount);
        
        M5.Lcd.setTextSize(3);
        
        // 心拍数 💗
        M5.Lcd.fillRect(TEXT_VALUE_X, 40, TEXT_VALUE_W, 25, BLACK);
        M5.Lcd.setCursor(TEXT_VALUE_X, 40);
        if (hr > 0 && hr < 200) {
            M5.Lcd.setTextColor(GREEN);
            M5.Lcd.print(hr, 0);
        } else {
            M5.Lcd.setTextColor(YELLOW);
            M5.Lcd.print("--");
        }
        M5.Lcd.setTextColor(WHITE);

        // SpO2 🫁
        M5.Lcd.fillRect(TEXT_VALUE_X, 90, TEXT_VALUE_W, 25, BLACK);
        M5.Lcd.setCursor(TEXT_VALUE_X, 90);
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

        lastHeartRate = hr;
        requestFaceRedraw = true;

        tsLastReport = millis();
    }
}
