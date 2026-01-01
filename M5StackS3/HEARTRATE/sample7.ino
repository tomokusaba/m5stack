/*
 * 使用ユニット / Units Used:
 *   - MAX30100 心拍センサー / Heart Rate Sensor (I2C)
 *   - NECO Unit (NeoPixel LED)
 *   - microSD カードリーダー / microSD Card Reader (SPI)
 * 
 * ピン配置 / Pin Assignment:
 *   - MAX30100: PortB I2C (SDA=9, SCL=8)
 *   - NECO Unit: PortA (GPIO 2)
 *   - microSD: SPI (CS=4, SCK=36, MISO=35, MOSI=37)
 */

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

#define PIN        2 // PortA 🐱
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

// 🌈 かわいいパステルカラーパレット（猫耳用）
const uint8_t CUTE_COLORS[][3] = {
    {255, 182, 193},  // ライトピンク 💗
    {255, 105, 180},  // ホットピンク 💖
    {238, 130, 238},  // バイオレット 💜
    {221, 160, 221},  // プラム 🪻
    {173, 216, 230},  // ライトブルー 💙
    {135, 206, 250},  // スカイブルー 🩵
    {255, 218, 185},  // ピーチ 🍑
    {255, 192, 203},  // ピンク 🎀
    {230, 230, 250},  // ラベンダー 💐
    {255, 160, 122},  // ライトサーモン 🧡
};
const int NUM_CUTE_COLORS = 10;

// 🎭 エフェクトモード管理
enum NekoEffect {
    EFFECT_HEARTBEAT,      // 💓 ハートビート
    EFFECT_RAINBOW_WAVE,   // 🌈 虹の波
    EFFECT_SHOOTING_STAR,  // 🌠 流れ星
    EFFECT_BREATHING,      // 😺 呼吸するような明滅
    EFFECT_SPARKLE_RAIN,   // ✨ キラキラの雨
    EFFECT_AURORA,         // 🌌 オーロラ
    EFFECT_NYAN_CAT,       // 🐱 にゃんキャット風
    EFFECT_COUNT
};
int currentEffect = 0;

// ✨ キラキラエフェクト用
uint8_t sparklePositions[12];
uint8_t sparkleBrightness[12];
uint32_t lastSparkleUpdate = 0;

// 🌠 流れ星エフェクト用
int shootingStarPos = 0;
int shootingStarTail = 8;

// 😺 呼吸エフェクト用
float breathPhase = 0;

// 🌌 オーロラ用
float auroraOffset = 0;

// 🌟 HSVからRGBに変換（虹色グラデーション用）
void hsvToRgb(float h, float s, float v, uint8_t* r, uint8_t* g, uint8_t* b) {
    int i = (int)(h * 6);
    float f = h * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);
    
    float rf, gf, bf;
    switch (i % 6) {
        case 0: rf = v; gf = t; bf = p; break;
        case 1: rf = q; gf = v; bf = p; break;
        case 2: rf = p; gf = v; bf = t; break;
        case 3: rf = p; gf = q; bf = v; break;
        case 4: rf = t; gf = p; bf = v; break;
        case 5: rf = v; gf = p; bf = q; break;
    }
    *r = (uint8_t)(rf * 255);
    *g = (uint8_t)(gf * 255);
    *b = (uint8_t)(bf * 255);
}

// 🎆 イージング関数（なめらかなアニメーション用）
float easeInOutSine(float t) {
    return -(cos(PI * t) - 1) / 2;
}

// 💓 ハートビートエフェクト
void effectHeartbeat(bool isBeat) {
    int center = NUMPIXELS / 2;
    int pulsePos = (beatCount * 7) % (NUMPIXELS / 2);
    
    for (int i = 0; i < NUMPIXELS; i++) {
        int distFromCenter = abs(i - center);
        float intensity = 1.0f - ((float)distFromCenter / (NUMPIXELS / 2));
        intensity = max(0.1f, intensity);
        
        bool isPulse = (abs(distFromCenter - pulsePos) < 4);
        
        uint8_t r, g, b;
        if (isBeat) {
            if (isPulse) {
                r = 255 * 0.9; g = 80 * 0.9; b = 150 * 0.9;
            } else {
                r = (uint8_t)(255 * intensity * 0.6);
                g = (uint8_t)(100 * intensity * 0.3);
                b = (uint8_t)(180 * intensity * 0.5);
            }
        } else {
            r = (uint8_t)(180 * intensity * 0.3);
            g = (uint8_t)(80 * intensity * 0.2);
            b = (uint8_t)(120 * intensity * 0.25);
        }
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
    
    // 耳先端のハイライト ✨
    if (isBeat) {
        pixels.setPixelColor(0, pixels.Color(255, 150, 200));
        pixels.setPixelColor(NUMPIXELS - 1, pixels.Color(255, 150, 200));
    }
}

// 🌈 虹の波エフェクト
void effectRainbowWave() {
    float hueOffset = (float)(beatCount * 15 % 360) / 360.0f;
    
    for (int i = 0; i < NUMPIXELS; i++) {
        float hue = fmod(hueOffset + (float)i / NUMPIXELS, 1.0f);
        uint8_t r, g, b;
        hsvToRgb(hue, 0.8f, 0.5f, &r, &g, &b);
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
    
    // キラキラオーバーレイ ✨
    for (int j = 0; j < 3; j++) {
        int pos = (beatCount * 5 + j * 23) % NUMPIXELS;
        pixels.setPixelColor(pos, pixels.Color(255, 255, 255));
    }
}

// 🌠 流れ星エフェクト
void effectShootingStar() {
    pixels.clear();
    
    // 複数の流れ星を生成
    for (int star = 0; star < 3; star++) {
        int starPos = (shootingStarPos + star * 25) % NUMPIXELS;
        
        // メインの星 ⭐
        pixels.setPixelColor(starPos, pixels.Color(255, 255, 220));
        
        // 尾を描画（グラデーション）🌟
        for (int t = 1; t < shootingStarTail; t++) {
            int tailPos = (starPos - t + NUMPIXELS) % NUMPIXELS;
            float fade = 1.0f - ((float)t / shootingStarTail);
            fade = fade * fade; // 二次関数で急速に減衰
            
            // 尾はピンク〜紫のグラデーション
            uint8_t r = (uint8_t)(255 * fade);
            uint8_t g = (uint8_t)(150 * fade * 0.5f);
            uint8_t b = (uint8_t)(255 * fade * 0.8f);
            pixels.setPixelColor(tailPos, pixels.Color(r, g, b));
        }
    }
    
    shootingStarPos = (shootingStarPos + 3) % NUMPIXELS;
}

// 😺 呼吸するような明滅エフェクト
void effectBreathing() {
    breathPhase += 0.15f;
    float breath = (sin(breathPhase) + 1.0f) / 2.0f; // 0〜1
    breath = easeInOutSine(breath); // なめらかに
    
    // 心拍数に応じた色味（低いと青、高いとピンク）
    float hrRatio = constrain(lastHeartRate / 120.0f, 0.0f, 1.0f);
    
    for (int i = 0; i < NUMPIXELS; i++) {
        // 位置による微妙な波打ち
        float posWave = sin((float)i / 10.0f + breathPhase * 0.5f) * 0.2f + 0.8f;
        float intensity = breath * posWave;
        
        // 色のブレンド（青〜ピンク）
        uint8_t r = (uint8_t)(lerp(100, 255, hrRatio) * intensity * 0.5f);
        uint8_t g = (uint8_t)(lerp(180, 120, hrRatio) * intensity * 0.3f);
        uint8_t b = (uint8_t)(lerp(255, 200, hrRatio) * intensity * 0.5f);
        
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
}

// ✨ キラキラの雨エフェクト
void effectSparkleRain() {
    // 背景のパステルグラデーション
    for (int i = 0; i < NUMPIXELS; i++) {
        int colorIndex = (i + beatCount) % NUM_CUTE_COLORS;
        uint8_t r = CUTE_COLORS[colorIndex][0] * 0.15f;
        uint8_t g = CUTE_COLORS[colorIndex][1] * 0.15f;
        uint8_t b = CUTE_COLORS[colorIndex][2] * 0.15f;
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
    
    // ランダムにキラキラを更新
    if (millis() - lastSparkleUpdate > 50) {
        lastSparkleUpdate = millis();
        for (int j = 0; j < 12; j++) {
            if (sparkleBrightness[j] > 0) {
                sparkleBrightness[j] -= 25;
            } else if (random(100) < 20) {
                sparklePositions[j] = random(NUMPIXELS);
                sparkleBrightness[j] = 255;
            }
        }
    }
    
    // キラキラを描画
    for (int j = 0; j < 12; j++) {
        if (sparkleBrightness[j] > 0) {
            float b = sparkleBrightness[j] / 255.0f;
            pixels.setPixelColor(sparklePositions[j], 
                pixels.Color((uint8_t)(255 * b), (uint8_t)(255 * b), (uint8_t)(255 * b)));
        }
    }
}

// 🌌 オーロラエフェクト
void effectAurora() {
    auroraOffset += 0.08f;
    
    for (int i = 0; i < NUMPIXELS; i++) {
        // 複数の波を重ね合わせる
        float wave1 = sin(auroraOffset + (float)i * 0.15f) * 0.5f + 0.5f;
        float wave2 = sin(auroraOffset * 0.7f + (float)i * 0.1f) * 0.5f + 0.5f;
        float wave3 = sin(auroraOffset * 1.3f + (float)i * 0.2f) * 0.5f + 0.5f;
        
        // 色をブレンド（緑、青、紫、ピンク）
        uint8_t r = (uint8_t)((wave1 * 150 + wave3 * 100) * 0.4f);
        uint8_t g = (uint8_t)((wave2 * 255) * 0.35f);
        uint8_t b = (uint8_t)((wave1 * 200 + wave2 * 150) * 0.5f);
        
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
}

// 🐱 にゃんキャット風エフェクト（レインボー＋リズム）
void effectNyanCat(bool isBeat) {
    float hueBase = (float)(millis() / 20 % 360) / 360.0f;
    
    for (int i = 0; i < NUMPIXELS; i++) {
        float hue = fmod(hueBase + (float)i / (NUMPIXELS / 2.0f), 1.0f);
        
        // ビート時は明るく、通常時は少し暗く
        float brightness = isBeat ? 0.7f : 0.35f;
        
        // 波打つ明るさ
        float wave = sin((float)i * 0.3f + millis() / 100.0f) * 0.2f + 0.8f;
        brightness *= wave;
        
        uint8_t r, g, b;
        hsvToRgb(hue, 0.9f, brightness, &r, &g, &b);
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
    
    // 🌟 にゃんの目（両端にキラキラ）
    if (isBeat) {
        pixels.setPixelColor(0, pixels.Color(255, 255, 200));
        pixels.setPixelColor(1, pixels.Color(255, 200, 150));
        pixels.setPixelColor(NUMPIXELS - 1, pixels.Color(255, 255, 200));
        pixels.setPixelColor(NUMPIXELS - 2, pixels.Color(255, 200, 150));
    }
}

void onBeatDetected()
{
    beatCount++;
    Serial.print("💓 Beat #");
    Serial.print(beatCount);
    Serial.print(" | Effect: ");
    Serial.println(currentEffect);
    
    // 🎭 10ビートごとにエフェクト変更！
    if (beatCount % 10 == 0) {
        currentEffect = (currentEffect + 1) % EFFECT_COUNT;
        Serial.print("🎭 Effect changed to: ");
        Serial.println(currentEffect);
    }
    
    pixels.clear();
    
    // ハートアイコン描画
    if (beatflg) {
        M5.Lcd.fillCircle(30, 40, 10, BLACK);
        M5.Lcd.fillCircle(50, 40, 10, BLACK);
        M5.Lcd.fillCircle(40, 41, 3, BLACK);
        M5.Lcd.fillTriangle(22, 45, 58, 45, 40, 65, BLACK);
        beatflg = false;
    } else {
        M5.Lcd.fillCircle(30, 40, 10, RED);
        M5.Lcd.fillCircle(50, 40, 10, RED);
        M5.Lcd.fillCircle(40, 41, 3, RED);
        M5.Lcd.fillTriangle(22, 45, 58, 45, 40, 65, RED);
        beatflg = true;
    }
    
    // 🐱 現在のエフェクトを実行
    switch (currentEffect) {
        case EFFECT_HEARTBEAT:
            effectHeartbeat(beatflg);
            break;
        case EFFECT_RAINBOW_WAVE:
            effectRainbowWave();
            break;
        case EFFECT_SHOOTING_STAR:
            effectShootingStar();
            break;
        case EFFECT_BREATHING:
            effectBreathing();
            break;
        case EFFECT_SPARKLE_RAIN:
            effectSparkleRain();
            break;
        case EFFECT_AURORA:
            effectAurora();
            break;
        case EFFECT_NYAN_CAT:
            effectNyanCat(beatflg);
            break;
        default:
            effectHeartbeat(beatflg);
            break;
    }
    
    pixels.show();

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
    // CoreS3 PortB: G9=SDA, G8=SCL
    Serial.println("📍 I2C PortB (SDA=9, SCL=8)");
    Wire.begin(9, 8);  // Wire.begin(SDA, SCL)
    delay(500);

    
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
