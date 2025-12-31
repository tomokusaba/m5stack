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

#define PIN        17 // PortC 🐱
#define NUMPIXELS 70
Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);

#define SAMPLING_RATE   (MAX30100_SAMPRATE_100HZ)
#define IR_LED_CURRENT  (MAX30100_LED_CURR_24MA)
#define RED_LED_CURRENT (MAX30100_LED_CURR_27_1MA)
#define PULSE_WIDTH     (MAX30100_SPC_PW_1600US_16BITS)
#define HIGHRES_MODE    (true)
#define REPORTING_PERIOD_MS 1000
#define HEALTH_CHECK_MS     5000  // 5秒ごとにヘルスチェック 🔍

#define MAX30100_I2C_ADDRESS 0x57  // MAX30100のI2Cアドレス 📡

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
uint32_t tsLastHealthCheck = 0;
bool beatflg = false;
int beatCount = 0;
int reinitCount = 0;
float lastHeartRate = 0.0f;
bool sdReady = false;
volatile bool requestFaceRedraw = false;
bool kameSpriteReady = false;

// 🎮 IMUデータ（グローバル）
float imuAccelX = 0, imuAccelY = 0, imuAccelZ = 0;      // 加速度 📐
float imuGyroX = 0, imuGyroY = 0, imuGyroZ = 0;         // ジャイロ 🔄
float imuMagX = 0, imuMagY = 0, imuMagZ = 0;            // 磁力計 🧭
float imuBaseHue = 0;          // IMUから計算した基本色相 🌈
float imuBrightness = 0.5f;    // IMUから計算した明るさ ✨
float imuSaturation = 0.8f;    // IMUから計算した彩度 🎨
bool imuReady = false;         // IMU初期化済みフラグ

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

// 😺 呼吸エフェクト用
float breathPhase = 0;

// 🌌 オーロラ用
float auroraOffset = 0;

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

// 🌟 HSVからRGBに変換（IMUカラー用）
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
        default: rf = v; gf = t; bf = p; break;
    }
    *r = (uint8_t)(rf * 255);
    *g = (uint8_t)(gf * 255);
    *b = (uint8_t)(bf * 255);
}

// 🎆 イージング関数（なめらかなアニメーション用）
float easeInOutSine(float t) {
    return -(cos(PI * t) - 1) / 2;
}

// 🎮 IMUデータを更新してカラー計算
void updateIMUColor() {
    if (!imuReady) return;
    
    auto imu_update = CoreS3.Imu.update();
    if (imu_update) {
        auto data = CoreS3.Imu.getImuData();
        
        // 加速度データ取得 📐
        imuAccelX = data.accel.x;
        imuAccelY = data.accel.y;
        imuAccelZ = data.accel.z;
        
        // ジャイロデータ取得 🔄
        imuGyroX = data.gyro.x;
        imuGyroY = data.gyro.y;
        imuGyroZ = data.gyro.z;
        
        // 磁力計データ取得 🧭（存在する場合）
        // BMM150からのデータ
        imuMagX = data.mag.x;
        imuMagY = data.mag.y;
        imuMagZ = data.mag.z;
        
        // 🌈 色相をIMUデータから計算
        // 磁力計の向き（コンパス）で基本色相を決定
        float heading = atan2(imuMagY, imuMagX);  // -π ~ π
        imuBaseHue = (heading + PI) / (2 * PI);   // 0 ~ 1 に正規化
        
        // 加速度から傾きを計算して色相をオフセット
        float tiltX = atan2(imuAccelX, imuAccelZ);
        float tiltY = atan2(imuAccelY, imuAccelZ);
        float tiltOffset = (tiltX + tiltY) / (4 * PI);  // 小さめのオフセット
        imuBaseHue = fmod(imuBaseHue + tiltOffset + 1.0f, 1.0f);
        
        // ✨ 明るさをジャイロの回転速度から計算
        float gyroMagnitude = sqrt(imuGyroX * imuGyroX + imuGyroY * imuGyroY + imuGyroZ * imuGyroZ);
        imuBrightness = constrain(0.3f + gyroMagnitude / 500.0f, 0.3f, 1.0f);
        
        // 🎨 彩度を加速度の大きさから計算
        float accelMagnitude = sqrt(imuAccelX * imuAccelX + imuAccelY * imuAccelY + imuAccelZ * imuAccelZ);
        float accelDeviation = abs(accelMagnitude - 1.0f);  // 1Gからの偏差
        imuSaturation = constrain(0.6f + accelDeviation * 0.4f, 0.6f, 1.0f);
    }
}

// 🎨 IMUベースの色を取得（位置オフセット付き）
void getIMUColor(int pixelIndex, float brightnessMod, uint8_t* r, uint8_t* g, uint8_t* b) {
    // 位置に応じた色相オフセット（グラデーション効果）
    float posOffset = (float)pixelIndex / NUMPIXELS * 0.3f;
    float hue = fmod(imuBaseHue + posOffset, 1.0f);
    
    float brightness = imuBrightness * brightnessMod;
    brightness = constrain(brightness, 0.0f, 1.0f);
    
    hsvToRgb(hue, imuSaturation, brightness, r, g, b);
}

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

// 🐱 各エフェクト関数（IMUカラーベース）

// 💓 ハートビートエフェクト（IMUで色が変わる！）
static int pulseWave = 0;
void effectHeartbeat(bool isBeat) {
    int center = NUMPIXELS / 2;
    
    if (isBeat) {
        pulseWave = 0;
    } else {
        pulseWave = min(pulseWave + 5, NUMPIXELS / 2);
    }
    
    float baseBrightness = isBeat ? 1.0f : 0.4f;
    
    for (int i = 0; i < NUMPIXELS; i++) {
        int distFromCenter = abs(i - center);
        float intensity = 1.0f - ((float)distFromCenter / (NUMPIXELS / 2));
        intensity = max(0.15f, intensity);
        
        bool isOnWave = (abs(distFromCenter - pulseWave) < 5);
        
        uint8_t r, g, b;
        if (isOnWave && !isBeat) {
            float waveFade = 1.0f - (float)abs(distFromCenter - pulseWave) / 5.0f;
            getIMUColor(i, waveFade * 1.2f, &r, &g, &b);
        } else {
            getIMUColor(i, intensity * baseBrightness, &r, &g, &b);
        }
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
    
    // ✨ 耳先端
    uint8_t tr, tg, tb;
    getIMUColor(0, isBeat ? 1.0f : 0.5f, &tr, &tg, &tb);
    pixels.setPixelColor(0, pixels.Color(tr, tg, tb));
    pixels.setPixelColor(NUMPIXELS - 1, pixels.Color(tr, tg, tb));
}

// 🌈 虹の波エフェクト（IMU + 虹）
static float rainbowPulse = 0.3f;
void effectRainbowWave(bool isBeat) {
    if (isBeat) {
        rainbowPulse = 0.9f;
    } else {
        rainbowPulse = max(0.3f, rainbowPulse - 0.08f);
    }
    
    for (int i = 0; i < NUMPIXELS; i++) {
        // IMUの色相をベースに虹のオフセットを追加
        float posOffset = (float)i / NUMPIXELS;
        float hue = fmod(imuBaseHue + posOffset + millis() / 5000.0f, 1.0f);
        float wave = sin((float)i * 0.2f + millis() / 200.0f) * 0.15f + 0.85f;
        float brightness = rainbowPulse * wave * imuBrightness;
        
        uint8_t r, g, b;
        hsvToRgb(hue, imuSaturation, brightness, &r, &g, &b);
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
    
    // ✨ キラキラ
    int sparkleCount = isBeat ? 8 : 3;
    for (int j = 0; j < sparkleCount; j++) {
        int pos = (millis() / 30 + j * 17) % NUMPIXELS;
        pixels.setPixelColor(pos, pixels.Color(255, 255, 255));
    }
}

// 🌠 流れ星エフェクト（IMUで色＆速度変化）
static int starSpeed = 2;
static int numStars = 2;
void effectShootingStar(bool isBeat) {
    // ジャイロ回転で速度変化
    float gyroSpeed = sqrt(imuGyroX * imuGyroX + imuGyroY * imuGyroY) / 100.0f;
    
    if (isBeat) {
        starSpeed = 8;
        numStars = 5;
    } else {
        starSpeed = max(2, (int)(2 + gyroSpeed));
        numStars = max(2, numStars - 1);
    }
    
    // 背景
    for (int i = 0; i < NUMPIXELS; i++) {
        uint8_t r, g, b;
        getIMUColor(i, 0.1f, &r, &g, &b);
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
    
    // 流れ星
    for (int star = 0; star < numStars; star++) {
        int starPos = (shootingStarPos + star * (NUMPIXELS / numStars)) % NUMPIXELS;
        
        pixels.setPixelColor(starPos, pixels.Color(255, 255, 255));
        
        int tailLen = isBeat ? 12 : 8;
        for (int t = 1; t < tailLen; t++) {
            int tailPos = (starPos - t + NUMPIXELS) % NUMPIXELS;
            float fade = 1.0f - ((float)t / tailLen);
            fade = fade * fade;
            
            uint8_t r, g, b;
            getIMUColor(tailPos, fade, &r, &g, &b);
            pixels.setPixelColor(tailPos, pixels.Color(r, g, b));
        }
    }
    
    shootingStarPos = (shootingStarPos + starSpeed) % NUMPIXELS;
}

// 😺 呼吸エフェクト（IMUで色変化）
static float beatFlash = 0;
void effectBreathing(bool isBeat) {
    breathPhase += 0.12f;
    float breath = (sin(breathPhase) + 1.0f) / 2.0f;
    breath = easeInOutSine(breath);
    
    if (isBeat) {
        beatFlash = 1.0f;
    } else {
        beatFlash = max(0.0f, beatFlash - 0.15f);
    }
    
    float totalIntensity = max(breath * 0.6f, beatFlash);
    
    for (int i = 0; i < NUMPIXELS; i++) {
        float posWave = sin((float)i / 8.0f + breathPhase * 0.5f) * 0.2f + 0.8f;
        float intensity = totalIntensity * posWave;
        
        uint8_t r, g, b;
        getIMUColor(i, intensity, &r, &g, &b);
        
        // ビート時は白っぽく
        if (beatFlash > 0.3f) {
            r = min(255, (int)(r + 50 * beatFlash));
            g = min(255, (int)(g + 50 * beatFlash));
            b = min(255, (int)(b + 50 * beatFlash));
        }
        
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
}

// ✨ キラキラの雨エフェクト（IMUカラー）
static float sparkleBurst = 0;
void effectSparkleRain(bool isBeat) {
    if (isBeat) {
        sparkleBurst = 1.0f;
        for (int j = 0; j < 12; j++) {
            sparklePositions[j] = random(NUMPIXELS);
            sparkleBrightness[j] = 255;
        }
    } else {
        sparkleBurst = max(0.0f, sparkleBurst - 0.1f);
    }
    
    float bgBright = 0.15f + sparkleBurst * 0.3f;
    for (int i = 0; i < NUMPIXELS; i++) {
        uint8_t r, g, b;
        getIMUColor(i, bgBright, &r, &g, &b);
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
    
    int spawnChance = sparkleBurst > 0.3f ? 50 : 15;
    if (millis() - lastSparkleUpdate > 40) {
        lastSparkleUpdate = millis();
        for (int j = 0; j < 12; j++) {
            if (sparkleBrightness[j] > 0) {
                sparkleBrightness[j] = (sparkleBrightness[j] > 30) ? sparkleBrightness[j] - 30 : 0;
            } else if (random(100) < spawnChance) {
                sparklePositions[j] = random(NUMPIXELS);
                sparkleBrightness[j] = 255;
            }
        }
    }
    
    for (int j = 0; j < 12; j++) {
        if (sparkleBrightness[j] > 0) {
            float bright = sparkleBrightness[j] / 255.0f;
            uint8_t r, g, b;
            getIMUColor(sparklePositions[j], bright, &r, &g, &b);
            pixels.setPixelColor(sparklePositions[j], pixels.Color(r, g, b));
        }
    }
}

// 🌌 オーロラエフェクト（IMU + オーロラ）
static float auroraSpeed = 0.08f;
static float auroraBright = 0.4f;
void effectAurora(bool isBeat) {
    if (isBeat) {
        auroraSpeed = 0.25f;
        auroraBright = 0.9f;
    } else {
        auroraSpeed = max(0.08f, auroraSpeed - 0.02f);
        auroraBright = max(0.4f, auroraBright - 0.06f);
    }
    
    auroraOffset += auroraSpeed;
    
    for (int i = 0; i < NUMPIXELS; i++) {
        float wave1 = sin(auroraOffset + (float)i * 0.15f) * 0.5f + 0.5f;
        float wave2 = sin(auroraOffset * 0.7f + (float)i * 0.1f) * 0.5f + 0.5f;
        
        // IMUカラーとオーロラをブレンド
        float brightness = (wave1 + wave2) * 0.5f * auroraBright * imuBrightness;
        
        uint8_t r, g, b;
        getIMUColor(i, brightness, &r, &g, &b);
        
        // オーロラっぽい緑を少し追加
        g = min(255, (int)(g + wave2 * 50 * auroraBright));
        
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
}

// 🐱 にゃんキャット風エフェクト（IMU虹）
static float nyanPulse = 0.35f;
static float nyanSpeed = 20.0f;
void effectNyanCat(bool isBeat) {
    if (isBeat) {
        nyanPulse = 1.0f;
        nyanSpeed = 8.0f;
    } else {
        nyanPulse = max(0.35f, nyanPulse - 0.08f);
        nyanSpeed = min(20.0f, nyanSpeed + 1.5f);
    }
    
    float hueBase = imuBaseHue + (float)(millis() / (int)nyanSpeed % 360) / 360.0f;
    
    for (int i = 0; i < NUMPIXELS; i++) {
        float hue = fmod(hueBase + (float)i / (NUMPIXELS / 3.0f), 1.0f);
        float wave = sin((float)i * 0.25f + millis() / 80.0f) * 0.25f + 0.75f;
        float brightness = nyanPulse * wave * imuBrightness;
        
        uint8_t r, g, b;
        hsvToRgb(hue, imuSaturation, brightness, &r, &g, &b);
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
    
    // 🌟 にゃんの目
    uint8_t eyeBright = (uint8_t)(150 + nyanPulse * 105);
    pixels.setPixelColor(0, pixels.Color(eyeBright, eyeBright, eyeBright * 0.8f));
    pixels.setPixelColor(NUMPIXELS - 1, pixels.Color(eyeBright, eyeBright, eyeBright * 0.8f));
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
    
    // IMUカラーを更新 🎮
    updateIMUColor();
    
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
            effectRainbowWave(beatflg);
            break;
        case EFFECT_SHOOTING_STAR:
            effectShootingStar(beatflg);
            break;
        case EFFECT_BREATHING:
            effectBreathing(beatflg);
            break;
        case EFFECT_SPARKLE_RAIN:
            effectSparkleRain(beatflg);
            break;
        case EFFECT_AURORA:
            effectAurora(beatflg);
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

    // 🎮 IMU初期化 (BMI270 + BMM150) - 内部I2C (SDA=G12, SCL=G11)
    Serial.println("🎮 Initializing IMU (BMI270+BMM150)...");
    Serial.println("   Internal I2C: SDA=G12, SCL=G11");
    
    // M5CoreS3のIMUはM5.begin()後にM5.Imu.begin()で初期化
    // CoreS3.Imu または M5.Imu を使用
    if (CoreS3.Imu.begin()) {
        imuReady = true;
        Serial.println("✅ IMU ready! (BMI270 @ 0x69)");
    } else {
        imuReady = false;
        Serial.println("⚠️ IMU init failed");
    }
    
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

    // ビート検出後の描画（コールバック外で実行してセンサー更新頻度を確保）
    if (requestFaceRedraw) {
        requestFaceRedraw = false;
        drawFace(lastHeartRate, beatflg);
    }
    
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
        M5.Lcd.print(" Alive:");
        M5.Lcd.print(alive ? "Y" : "N");
        M5.Lcd.print(" LED:");
        M5.Lcd.print(ledOn ? "Y" : "N");
        
        // 🎮 IMU情報を画面下部に表示
        M5.Lcd.fillRect(0, 200, 320, 40, BLACK);
        M5.Lcd.setTextSize(1);
        
        // エフェクト名を表示
        const char* effectNames[] = {"HEART", "RAINBOW", "STAR", "BREATH", "SPARKLE", "AURORA", "NYAN"};
        M5.Lcd.setCursor(0, 200);
        M5.Lcd.setTextColor(MAGENTA);
        M5.Lcd.print("Effect:");
        M5.Lcd.print(effectNames[currentEffect]);
        
        // IMUカラー情報（Hue/Sat/Bright）
        M5.Lcd.setCursor(100, 200);
        M5.Lcd.setTextColor(CYAN);
        M5.Lcd.print("H:");
        M5.Lcd.print((int)(imuBaseHue * 360));
        M5.Lcd.print(" S:");
        M5.Lcd.print((int)(imuSaturation * 100));
        M5.Lcd.print(" B:");
        M5.Lcd.print((int)(imuBrightness * 100));
        
        // ジャイロ（回転速度）
        M5.Lcd.setCursor(0, 210);
        M5.Lcd.setTextColor(YELLOW);
        M5.Lcd.print("Gyro X:");
        M5.Lcd.print((int)imuGyroX);
        M5.Lcd.print(" Y:");
        M5.Lcd.print((int)imuGyroY);
        M5.Lcd.print(" Z:");
        M5.Lcd.print((int)imuGyroZ);
        
        // 加速度（傾き）
        M5.Lcd.setCursor(0, 220);
        M5.Lcd.setTextColor(GREEN);
        M5.Lcd.print("Accel X:");
        M5.Lcd.print(imuAccelX, 1);
        M5.Lcd.print(" Y:");
        M5.Lcd.print(imuAccelY, 1);
        M5.Lcd.print(" Z:");
        M5.Lcd.print(imuAccelZ, 1);
        
        // 磁力計（コンパス）
        M5.Lcd.setCursor(0, 230);
        M5.Lcd.setTextColor(ORANGE);
        M5.Lcd.print("Mag X:");
        M5.Lcd.print((int)imuMagX);
        M5.Lcd.print(" Y:");
        M5.Lcd.print((int)imuMagY);
        M5.Lcd.print(" Z:");
        M5.Lcd.print((int)imuMagZ);
        
        M5.Lcd.setTextColor(WHITE);

        lastHeartRate = hr;
        requestFaceRedraw = true;

        tsLastReport = millis();
    }
    
    // 🎮 IMUを常時更新（リアルタイム反映）
    updateIMUColor();
}
