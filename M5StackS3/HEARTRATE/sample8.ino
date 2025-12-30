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

// 💓 ハートビートエフェクト（大きく脈打つ！）
void effectHeartbeat(bool isBeat) {
    int center = NUMPIXELS / 2;
    
    // 🫀 ビート時は中心から外へ広がる波！
    static int pulseWave = 0;
    if (isBeat) {
        pulseWave = 0;  // ビートで波をリセット
    } else {
        pulseWave = min(pulseWave + 5, NUMPIXELS / 2);  // 波が広がる
    }
    
    // 💗 全体の明るさも脈動
    float baseBrightness = isBeat ? 1.0f : 0.4f;
    
    for (int i = 0; i < NUMPIXELS; i++) {
        int distFromCenter = abs(i - center);
        float intensity = 1.0f - ((float)distFromCenter / (NUMPIXELS / 2));
        intensity = max(0.15f, intensity);
        
        // 🌊 波の位置にいるかチェック
        bool isOnWave = (abs(distFromCenter - pulseWave) < 5);
        
        uint8_t r, g, b;
        if (isOnWave && !isBeat) {
            // 波の部分は明るいピンク〜白
            float waveFade = 1.0f - (float)abs(distFromCenter - pulseWave) / 5.0f;
            r = (uint8_t)(255 * waveFade);
            g = (uint8_t)(150 * waveFade);
            b = (uint8_t)(200 * waveFade);
        } else if (isBeat) {
            // ビート瞬間は全体がパッと明るく！💥
            r = (uint8_t)(255 * intensity * baseBrightness);
            g = (uint8_t)(100 * intensity * baseBrightness);
            b = (uint8_t)(180 * intensity * baseBrightness);
        } else {
            // 通常時は落ち着いた色
            r = (uint8_t)(200 * intensity * 0.25f);
            g = (uint8_t)(80 * intensity * 0.15f);
            b = (uint8_t)(140 * intensity * 0.2f);
        }
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
    
    // ✨ 耳先端は常にキラキラ（ビート時はMAX）
    uint8_t tipBright = isBeat ? 255 : 100;
    pixels.setPixelColor(0, pixels.Color(tipBright, tipBright * 0.6, tipBright * 0.8));
    pixels.setPixelColor(1, pixels.Color(tipBright * 0.7, tipBright * 0.4, tipBright * 0.6));
    pixels.setPixelColor(NUMPIXELS - 1, pixels.Color(tipBright, tipBright * 0.6, tipBright * 0.8));
    pixels.setPixelColor(NUMPIXELS - 2, pixels.Color(tipBright * 0.7, tipBright * 0.4, tipBright * 0.6));
}

// 🌈 虹の波エフェクト（脈動する虹！）
static float rainbowPulse = 0.3f;
void effectRainbowWave(bool isBeat) {
    float hueOffset = (float)(millis() / 50 % 360) / 360.0f;
    
    // 💓 ビート時に明るさがバウンス！
    if (isBeat) {
        rainbowPulse = 0.9f;  // パッと明るく
    } else {
        rainbowPulse = max(0.3f, rainbowPulse - 0.08f);  // 徐々に暗く
    }
    
    for (int i = 0; i < NUMPIXELS; i++) {
        float hue = fmod(hueOffset + (float)i / NUMPIXELS, 1.0f);
        
        // 🌊 波打つような明るさの変化
        float wave = sin((float)i * 0.2f + millis() / 200.0f) * 0.15f + 0.85f;
        float brightness = rainbowPulse * wave;
        
        uint8_t r, g, b;
        hsvToRgb(hue, 0.85f, brightness, &r, &g, &b);
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
    
    // ✨ ビート時はキラキラ増量！
    int sparkleCount = isBeat ? 8 : 3;
    for (int j = 0; j < sparkleCount; j++) {
        int pos = (millis() / 30 + j * 17) % NUMPIXELS;
        pixels.setPixelColor(pos, pixels.Color(255, 255, 255));
    }
}

// 🌠 流れ星エフェクト（ビートで加速＆増殖！）
static int starSpeed = 2;
static int numStars = 2;
void effectShootingStar(bool isBeat) {
    // 💓 ビート時は星が加速＆増える！
    if (isBeat) {
        starSpeed = 8;  // 高速化
        numStars = 5;   // 星増量
    } else {
        starSpeed = max(2, starSpeed - 1);
        numStars = max(2, numStars - 1);
    }
    
    // 🌌 背景にうっすらパステル
    for (int i = 0; i < NUMPIXELS; i++) {
        float bgWave = sin((float)i * 0.1f + millis() / 500.0f) * 0.5f + 0.5f;
        uint8_t bg = (uint8_t)(20 * bgWave);
        pixels.setPixelColor(i, pixels.Color(bg, bg * 0.5f, bg * 0.8f));
    }
    
    // 🌠 流れ星を描画
    for (int star = 0; star < numStars; star++) {
        int starPos = (shootingStarPos + star * (NUMPIXELS / numStars)) % NUMPIXELS;
        
        // ⭐ メインの星（ビート時は大きく明るく）
        uint8_t starBright = isBeat ? 255 : 200;
        pixels.setPixelColor(starPos, pixels.Color(starBright, starBright, starBright * 0.9f));
        if (isBeat) {
            // 星の周りも光らせる
            int next = (starPos + 1) % NUMPIXELS;
            pixels.setPixelColor(next, pixels.Color(200, 200, 180));
        }
        
        // 🌟 尾を描画（ビート時は長い尾）
        int tailLen = isBeat ? 12 : 8;
        for (int t = 1; t < tailLen; t++) {
            int tailPos = (starPos - t + NUMPIXELS) % NUMPIXELS;
            float fade = 1.0f - ((float)t / tailLen);
            fade = fade * fade;
            
            // 尾はピンク〜紫〜青のグラデーション
            uint8_t r = (uint8_t)(255 * fade);
            uint8_t g = (uint8_t)(100 * fade * (1.0f - (float)t / tailLen));
            uint8_t b = (uint8_t)(255 * fade * 0.9f);
            pixels.setPixelColor(tailPos, pixels.Color(r, g, b));
        }
    }
    
    shootingStarPos = (shootingStarPos + starSpeed) % NUMPIXELS;
}

// 😺 呼吸するような明滅エフェクト（ビートでドキッ！）
static float beatFlash = 0;
void effectBreathing(bool isBeat) {
    breathPhase += 0.12f;
    float breath = (sin(breathPhase) + 1.0f) / 2.0f; // 0〜1
    breath = easeInOutSine(breath); // なめらかに
    
    // 💓 ビート時に「ドキッ」とフラッシュ！
    if (isBeat) {
        beatFlash = 1.0f;
    } else {
        beatFlash = max(0.0f, beatFlash - 0.15f);
    }
    
    // 心拍数に応じた色味（低いと青、高いとピンク）
    float hrRatio = constrain(lastHeartRate / 120.0f, 0.0f, 1.0f);
    
    // 🫀 ビートフラッシュと呼吸を合成
    float totalIntensity = max(breath * 0.6f, beatFlash);
    
    for (int i = 0; i < NUMPIXELS; i++) {
        // 位置による微妙な波打ち
        float posWave = sin((float)i / 8.0f + breathPhase * 0.5f) * 0.2f + 0.8f;
        float intensity = totalIntensity * posWave;
        
        // 色のブレンド（青〜ピンク）+ ビート時は白っぽく
        float flashWhite = beatFlash * 0.5f;
        uint8_t r = (uint8_t)((lerp(100, 255, hrRatio) * intensity + 255 * flashWhite) * 0.5f);
        uint8_t g = (uint8_t)((lerp(180, 120, hrRatio) * intensity + 200 * flashWhite) * 0.35f);
        uint8_t b = (uint8_t)((lerp(255, 200, hrRatio) * intensity + 220 * flashWhite) * 0.5f);
        
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
    
    // ✨ ビート時は両端がキラッ
    if (beatFlash > 0.5f) {
        pixels.setPixelColor(0, pixels.Color(255, 220, 240));
        pixels.setPixelColor(NUMPIXELS - 1, pixels.Color(255, 220, 240));
    }
}

// ✨ キラキラの雨エフェクト（ビートで花火のように爆発！）
static float sparkleBurst = 0;
void effectSparkleRain(bool isBeat) {
    // 💓 ビート時に背景が明るくなる
    if (isBeat) {
        sparkleBurst = 1.0f;
        // 全キラキラを一斉に発生！
        for (int j = 0; j < 12; j++) {
            sparklePositions[j] = random(NUMPIXELS);
            sparkleBrightness[j] = 255;
        }
    } else {
        sparkleBurst = max(0.0f, sparkleBurst - 0.1f);
    }
    
    // 🌸 背景のパステルグラデーション（ビート時は明るく）
    float bgBright = 0.15f + sparkleBurst * 0.3f;
    for (int i = 0; i < NUMPIXELS; i++) {
        int colorIndex = (i + (int)(millis() / 100)) % NUM_CUTE_COLORS;
        uint8_t r = CUTE_COLORS[colorIndex][0] * bgBright;
        uint8_t g = CUTE_COLORS[colorIndex][1] * bgBright;
        uint8_t b = CUTE_COLORS[colorIndex][2] * bgBright;
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
    
    // ランダムにキラキラを更新（ビート後は発生率UP）
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
    
    // ✨ キラキラを描画（ビート時は色付き）
    for (int j = 0; j < 12; j++) {
        if (sparkleBrightness[j] > 0) {
            float bright = sparkleBrightness[j] / 255.0f;
            if (sparkleBurst > 0.5f) {
                // ビート時はカラフルなキラキラ
                int c = j % NUM_CUTE_COLORS;
                pixels.setPixelColor(sparklePositions[j], 
                    pixels.Color((uint8_t)(CUTE_COLORS[c][0] * bright), 
                                 (uint8_t)(CUTE_COLORS[c][1] * bright), 
                                 (uint8_t)(CUTE_COLORS[c][2] * bright)));
            } else {
                pixels.setPixelColor(sparklePositions[j], 
                    pixels.Color((uint8_t)(255 * bright), (uint8_t)(255 * bright), (uint8_t)(255 * bright)));
            }
        }
    }
}

// 🌌 オーロラエフェクト（ビートで波が加速＆輝く！）
static float auroraSpeed = 0.08f;
static float auroraBright = 0.4f;
void effectAurora(bool isBeat) {
    // 💓 ビート時は波が加速＆明るく！
    if (isBeat) {
        auroraSpeed = 0.25f;
        auroraBright = 0.9f;
    } else {
        auroraSpeed = max(0.08f, auroraSpeed - 0.02f);
        auroraBright = max(0.4f, auroraBright - 0.06f);
    }
    
    auroraOffset += auroraSpeed;
    
    for (int i = 0; i < NUMPIXELS; i++) {
        // 複数の波を重ね合わせる
        float wave1 = sin(auroraOffset + (float)i * 0.15f) * 0.5f + 0.5f;
        float wave2 = sin(auroraOffset * 0.7f + (float)i * 0.1f) * 0.5f + 0.5f;
        float wave3 = sin(auroraOffset * 1.3f + (float)i * 0.2f) * 0.5f + 0.5f;
        
        // 🌊 ビート時の脈動を追加
        float pulse = isBeat ? 1.2f : 1.0f;
        
        // 色をブレンド（緑、青、紫、ピンク）
        uint8_t r = (uint8_t)((wave1 * 150 + wave3 * 120) * auroraBright * pulse);
        uint8_t g = (uint8_t)((wave2 * 255) * auroraBright * 0.9f * pulse);
        uint8_t b = (uint8_t)((wave1 * 200 + wave2 * 150) * auroraBright * pulse);
        
        // 明るさ制限
        r = min((uint8_t)255, r);
        g = min((uint8_t)255, g);
        b = min((uint8_t)255, b);
        
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
    
    // ✨ ビート時は耳先がキラリ
    if (isBeat) {
        pixels.setPixelColor(0, pixels.Color(200, 255, 220));
        pixels.setPixelColor(NUMPIXELS - 1, pixels.Color(200, 255, 220));
    }
}

// 🐱 にゃんキャット風エフェクト（レインボー＋大きな脈動！）
static float nyanPulse = 0.35f;
static float nyanSpeed = 20.0f;
void effectNyanCat(bool isBeat) {
    // 💓 ビート時は明るさ＆速度がドーン！
    if (isBeat) {
        nyanPulse = 1.0f;
        nyanSpeed = 8.0f;  // 高速化！
    } else {
        nyanPulse = max(0.35f, nyanPulse - 0.08f);
        nyanSpeed = min(20.0f, nyanSpeed + 1.5f);  // 徐々に戻る
    }
    
    float hueBase = (float)(millis() / (int)nyanSpeed % 360) / 360.0f;
    
    for (int i = 0; i < NUMPIXELS; i++) {
        float hue = fmod(hueBase + (float)i / (NUMPIXELS / 3.0f), 1.0f);
        
        // 🌊 大きく波打つ明るさ
        float wave = sin((float)i * 0.25f + millis() / 80.0f) * 0.25f + 0.75f;
        float brightness = nyanPulse * wave;
        
        // 🎵 ビート時は彩度も上げる
        float saturation = isBeat ? 1.0f : 0.85f;
        
        uint8_t r, g, b;
        hsvToRgb(hue, saturation, brightness, &r, &g, &b);
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
    
    // 🌟 にゃんの目（常に光るけどビート時はMAX！）
    uint8_t eyeBright = (uint8_t)(150 + nyanPulse * 105);
    pixels.setPixelColor(0, pixels.Color(eyeBright, eyeBright, eyeBright * 0.8f));
    pixels.setPixelColor(1, pixels.Color(eyeBright * 0.8f, eyeBright * 0.7f, eyeBright * 0.5f));
    pixels.setPixelColor(NUMPIXELS - 1, pixels.Color(eyeBright, eyeBright, eyeBright * 0.8f));
    pixels.setPixelColor(NUMPIXELS - 2, pixels.Color(eyeBright * 0.8f, eyeBright * 0.7f, eyeBright * 0.5f));
    
    // 🎀 ビート時は中央もキラリ
    if (nyanPulse > 0.7f) {
        int center = NUMPIXELS / 2;
        pixels.setPixelColor(center, pixels.Color(255, 255, 255));
        pixels.setPixelColor(center - 1, pixels.Color(255, 200, 220));
        pixels.setPixelColor(center + 1, pixels.Color(255, 200, 220));
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
    
    // 🐱 現在のエフェクトを実行（全てにビート状態を渡す！）
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
    
    // 📍 ポート配置情報を画面下部に表示
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(DARKGREY);
    M5.Lcd.setCursor(0, 220);
    M5.Lcd.print("PortA(G2):LED  PortB(G8,G9):I2C");
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
