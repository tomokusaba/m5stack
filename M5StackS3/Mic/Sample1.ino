// 🐱 猫耳LED マルチモード スケッチ
// タッチボタンで楽しいエフェクトを切り替え！ ✨

#include <M5CoreS3.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <arduinoFFT.h>  // 🎵 FFTライブラリ

#define PIN        2        // PortA 🐱
#define NUMPIXELS  70       // LED数

Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);

// モード定義 🎮
enum Mode {
    MODE_CHASE,      // 流れる光 🌊
    MODE_BLINK,      // 点滅 💡
    MODE_RAINBOW,    // 虹色グラデーション 🌈
    MODE_SPARKLE,    // キラキラ ✨
    MODE_BREATHE,    // 呼吸（フェード）💨
    MODE_PARTY,      // パーティー 🎉
    MODE_IMU,        // 🎮 IMUセンサー制御
    MODE_MIC,        // 🎤 マイク＋FFT制御
    MODE_COUNT       // モード数
};

// ボタン定義 📱
struct Button {
    int x, y, w, h;
    uint16_t color;
    const char* label;
    Mode mode;
};

// 画面下部にボタン配置（320x240画面）
Button buttons[] = {
    {  5, 195, 75, 40, 0x07E0, "CHASE",   MODE_CHASE},    // 緑
    { 85, 195, 75, 40, 0xFFE0, "BLINK",   MODE_BLINK},    // 黄
    {165, 195, 75, 40, 0xF81F, "RAINBW",  MODE_RAINBOW},  // マゼンタ
    {245, 195, 70, 40, 0x07FF, "SPARKL",  MODE_SPARKLE},  // シアン
    {  5, 150, 75, 40, 0xFD20, "BREATH",  MODE_BREATHE},  // オレンジ
    { 85, 150, 75, 40, 0xF800, "PARTY",   MODE_PARTY},    // 赤
    {165, 150, 75, 40, 0x001F, "IMU",     MODE_IMU},      // 🎮 青
    {245, 150, 70, 40, 0xFC00, "MIC",     MODE_MIC},      // 🎤 黄緑
};
const int BUTTON_COUNT = sizeof(buttons) / sizeof(buttons[0]);

// 状態変数
Mode currentMode = MODE_CHASE;
uint32_t lastUpdateTime = 0;
int animPosition = 0;
int hueOffset = 0;
bool blinkState = false;
float breathValue = 0;
float breathDir = 0.05;

// 🔄 自動モード切り替え用
uint32_t lastModeChangeTime = 0;
uint32_t modeChangeInterval = 10000;  // 10秒ごとに切り替え（ミリ秒）

// 🎮 IMUセンサー関連
bool imuReady = false;
float imuAccelX = 0, imuAccelY = 0, imuAccelZ = 0;  // 加速度 📐
float imuGyroX = 0, imuGyroY = 0, imuGyroZ = 0;     // ジャイロ 🔄
float imuMagX = 0, imuMagY = 0, imuMagZ = 0;        // 磁力計 🧭
float imuBaseHue = 0;        // 🎨 色相 (0~1)
float imuSaturation = 0.8f;  // ✨ 彩度 (0~1)
float imuBrightness = 0.7f;  // 💡 明るさ (0~1)

// 🎤 マイク＋FFT関連
#define SAMPLES 128              // FFTサンプル数（メモリ削減のため128に）
#define SAMPLING_FREQ 8000       // サンプリング周波数 (Hz) - 低めに設定

// FFT用配列（staticでグローバルに、floatでメモリ削減）
static float vReal[SAMPLES];
static float vImag[SAMPLES];
ArduinoFFT<float> FFT = ArduinoFFT<float>(vReal, vImag, SAMPLES, SAMPLING_FREQ);

// 🎤 マイクバッファ（staticでスタックオーバーフロー防止）
static int16_t micBuffer[SAMPLES];

// 🎵 音声分析結果
float bassLevel = 0;             // 低音域 (0~1)
float midLevel = 0;              // 中音域 (0~1)
float highLevel = 0;             // 高音域 (0~1)
float overallVolume = 0;         // 全体音量 (0~1)
float beatDetected = 0;          // ビート検出
float prevBassLevel = 0;         // 前回の低音域
uint32_t lastBeatTime = 0;       // 最後のビート時刻
bool micReady = false;           // マイク準備完了

// 🥁 ビート検出改善用（エネルギー履歴）
#define BEAT_HISTORY_SIZE 16     // 履歴サイズ
float bassHistory[BEAT_HISTORY_SIZE];  // 低音域の履歴
float midHistory[BEAT_HISTORY_SIZE];   // 中音域の履歴（スネア用）
int bassHistoryIndex = 0;        // 履歴インデックス
float bassAverage = 0;           // 低音域の平均
float bassVariance = 0;          // 低音域の分散
float midAverage = 0;            // 中音域の平均
float midVariance = 0;           // 中音域の分散
float prevMidLevel = 0;          // 前回の中音域
float beatThreshold = 0.2f;      // 適応的閾値
float midThreshold = 0.2f;       // 中音域閾値
bool useSnareMode = false;       // スネアモード（低音が常に高い場合）
int beatCount = 0;               // ビートカウント
float bpm = 0;                   // 推定BPM
uint32_t beatTimes[8];           // 最近8回のビート時刻
int beatTimeIndex = 0;           // ビート時刻インデックス

// 速度設定
int speeds[] = {30, 300, 20, 50, 30, 40, 30, 20};  // 各モードの更新間隔（MIC追加）

// HSVからRGBへ変換 🌈
uint32_t hsvToColor(int hue, float sat, float val) {
    hue = hue % 256;
    int region = hue / 43;
    int remainder = (hue - (region * 43)) * 6;
    
    int p = (int)(255 * val * (1 - sat));
    int q = (int)(255 * val * (1 - (sat * remainder / 255)));
    int t = (int)(255 * val * (1 - (sat * (255 - remainder) / 255)));
    int v = (int)(255 * val);
    
    switch (region) {
        case 0:  return pixels.Color(v, t, p);
        case 1:  return pixels.Color(q, v, p);
        case 2:  return pixels.Color(p, v, t);
        case 3:  return pixels.Color(p, q, v);
        case 4:  return pixels.Color(t, p, v);
        default: return pixels.Color(v, p, q);
    }
}

// 🌟 HSVからRGBに変換（IMUカラー用、float版）
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

// 🎮 IMUデータを更新してカラー計算
void updateIMUColor() {
    if (!imuReady) return;
    
    auto imu_update = M5.Imu.update();
    if (imu_update) {
        auto data = M5.Imu.getImuData();
        
        // 加速度データ取得 📐
        imuAccelX = data.accel.x;
        imuAccelY = data.accel.y;
        imuAccelZ = data.accel.z;
        
        // ジャイロデータ取得 🔄
        imuGyroX = data.gyro.x;
        imuGyroY = data.gyro.y;
        imuGyroZ = data.gyro.z;
        
        // 磁力計データ取得 🧭
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
        float gyroMagnitude = sqrt(imuGyroX * imuGyroX + 
                                   imuGyroY * imuGyroY + 
                                   imuGyroZ * imuGyroZ);
        imuBrightness = constrain(0.3f + gyroMagnitude / 500.0f, 0.3f, 1.0f);
        
        // 🎨 彩度を加速度の大きさから計算
        float accelMagnitude = sqrt(imuAccelX * imuAccelX + 
                                    imuAccelY * imuAccelY + 
                                    imuAccelZ * imuAccelZ);
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

// ボタン描画 🎨
void drawButtons() {
    for (int i = 0; i < BUTTON_COUNT; i++) {
        Button& btn = buttons[i];
        uint16_t bgColor = (btn.mode == currentMode) ? 0xFFFF : btn.color;
        uint16_t textColor = (btn.mode == currentMode) ? btn.color : BLACK;
        
        M5.Lcd.fillRoundRect(btn.x, btn.y, btn.w, btn.h, 8, bgColor);
        M5.Lcd.drawRoundRect(btn.x, btn.y, btn.w, btn.h, 8, WHITE);
        
        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(textColor);
        int textX = btn.x + (btn.w - strlen(btn.label) * 12) / 2;
        int textY = btn.y + (btn.h - 16) / 2;
        M5.Lcd.setCursor(textX, textY);
        M5.Lcd.print(btn.label);
    }
}

// タイトル描画 📺
void drawTitle() {
    M5.Lcd.fillRect(0, 0, 320, 145, BLACK);
    M5.Lcd.setTextSize(3);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(30, 5);
    M5.Lcd.print("NECO PARTY!");
    
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(CYAN);
    M5.Lcd.setCursor(50, 35);
    M5.Lcd.print("Mode: ");
    M5.Lcd.setTextColor(YELLOW);
    M5.Lcd.print(buttons[currentMode].label);
}

// 🎤 マイクデバッグ情報を描画（全モード対応）
void drawMicDebug() {
    // デバッグエリア（タイトル下、ボタン上）
    M5.Lcd.fillRect(0, 55, 320, 90, BLACK);
    
    // マイク状態
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(5, 58);
    M5.Lcd.setTextColor(micReady ? GREEN : RED);
    M5.Lcd.print("MIC: ");
    M5.Lcd.print(micReady ? "OK" : "NG");
    
    // サンプリングレート
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(60, 58);
    M5.Lcd.print("SR:");
    M5.Lcd.print(SAMPLING_FREQ);
    M5.Lcd.print("Hz");
    
    // ビート検出
    M5.Lcd.setCursor(140, 58);
    M5.Lcd.setTextColor(beatDetected > 0.3f ? YELLOW : DARKGREY);
    M5.Lcd.print("BEAT");
    if (beatDetected > 0.3f) {
        M5.Lcd.print("!");  // ビート時に!マーク
    }
    
    // BPM表示
    M5.Lcd.setCursor(185, 58);
    M5.Lcd.setTextColor(MAGENTA);
    M5.Lcd.print("BPM:");
    if (bpm > 0) {
        M5.Lcd.print((int)bpm);
    } else {
        M5.Lcd.print("---");
    }
    
    // 全体音量
    M5.Lcd.setCursor(250, 58);
    M5.Lcd.setTextColor(CYAN);
    M5.Lcd.print("V:");
    M5.Lcd.print((int)(overallVolume * 100));
    
    // === 周波数帯域バー表示 ===
    int barY = 72;
    int barHeight = 18;
    int barMaxWidth = 200;
    
    // 🔴 低音 (Bass) + 適応閾値マーカー
    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(5, barY + 2);
    M5.Lcd.print("BASS");
    int bassWidth = (int)(bassLevel * barMaxWidth);
    M5.Lcd.fillRect(45, barY, bassWidth, barHeight, RED);
    M5.Lcd.drawRect(45, barY, barMaxWidth, barHeight, DARKGREY);
    
    // 適応閾値マーカー（黄色の線）
    int thresholdX = 45 + (int)(beatThreshold * barMaxWidth);
    if (thresholdX < 45 + barMaxWidth) {
        M5.Lcd.drawFastVLine(thresholdX, barY, barHeight, YELLOW);
        M5.Lcd.drawFastVLine(thresholdX + 1, barY, barHeight, YELLOW);
    }
    
    M5.Lcd.setCursor(250, barY + 2);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.print((int)(bassLevel * 100));
    M5.Lcd.print("%");
    
    // 🟢 中音 (Mid)
    barY += barHeight + 4;
    M5.Lcd.setTextColor(GREEN);
    M5.Lcd.setCursor(5, barY + 2);
    M5.Lcd.print("MID ");
    int midWidth = (int)(midLevel * barMaxWidth);
    M5.Lcd.fillRect(45, barY, midWidth, barHeight, GREEN);
    M5.Lcd.drawRect(45, barY, barMaxWidth, barHeight, DARKGREY);
    M5.Lcd.setCursor(250, barY + 2);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.print((int)(midLevel * 100));
    M5.Lcd.print("%");
    
    // 🔵 高音 (High)
    barY += barHeight + 4;
    M5.Lcd.setTextColor(BLUE);
    M5.Lcd.setCursor(5, barY + 2);
    M5.Lcd.print("HIGH");
    int highWidth = (int)(highLevel * barMaxWidth);
    M5.Lcd.fillRect(45, barY, highWidth, barHeight, BLUE);
    M5.Lcd.drawRect(45, barY, barMaxWidth, barHeight, DARKGREY);
    M5.Lcd.setCursor(250, barY + 2);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.print((int)(highLevel * 100));
    M5.Lcd.print("%");
}

// 🔄 モード切り替え（リセット処理共通化）
void changeMode(Mode newMode) {
    currentMode = newMode;
    animPosition = 0;
    hueOffset = 0;
    breathValue = 0;
    breathDir = 0.05;
    
    Serial.print("🎮 Mode changed: ");
    Serial.println(buttons[currentMode].label);
    
    drawTitle();
    drawButtons();
}

// 🎲 ランダムモード切り替え
void randomModeChange() {
    Mode newMode;
    do {
        newMode = (Mode)random(MODE_COUNT);
    } while (newMode == currentMode);  // 同じモードは避ける
    
    changeMode(newMode);
    Serial.println("🔄 Auto random mode change!");
}

// ⏰ 周期的な自動切り替えチェック
void checkAutoModeChange() {
    if (millis() - lastModeChangeTime >= modeChangeInterval) {
        lastModeChangeTime = millis();
        randomModeChange();
    }
}

// タッチチェック 👆
void checkTouch() {
    M5.update();
    
    if (M5.Touch.getCount() > 0) {
        auto touch = M5.Touch.getDetail();
        if (touch.wasPressed()) {
            int tx = touch.x;
            int ty = touch.y;
            
            for (int i = 0; i < BUTTON_COUNT; i++) {
                Button& btn = buttons[i];
                if (tx >= btn.x && tx <= btn.x + btn.w &&
                    ty >= btn.y && ty <= btn.y + btn.h) {
                    changeMode(btn.mode);
                    lastModeChangeTime = millis();  // 手動切り替え時もタイマーリセット
                    break;
                }
            }
        }
    }
}

// === エフェクト関数 ===

// 🌊 チェイス（流れる光）+ IMU + MIC
void effectChase() {
    updateIMUColor();  // 🎮 IMU更新
    updateMicFFT();    // 🎤 マイク更新
    pixels.clear();
    
    // 🎤 マイクで尾の長さとスピードが変化
    int trailLen = 8 + (int)(bassLevel * 12);  // 8～20
    int speed = 1 + (int)(overallVolume * 3);   // 1～4
    
    // IMU + MIC ベースの色相を使用
    int baseHue = (int)(imuBaseHue * 256) + (int)(midLevel * 30);
    
    for (int i = 0; i < trailLen; i++) {
        int pos = (animPosition - i + NUMPIXELS) % NUMPIXELS;
        float brightness = 1.0 - ((float)i / trailLen);
        brightness = brightness * brightness * imuBrightness;
        
        // 🥁 ビート時にフラッシュ
        if (beatDetected > 0.3f && i < 3) {
            brightness = min(1.0f, brightness + beatDetected * 0.5f);
        }
        
        uint32_t color = hsvToColor((baseHue + pos * 4 + hueOffset) % 256, imuSaturation, brightness);
        pixels.setPixelColor(pos, color);
    }
    
    animPosition = (animPosition + speed) % NUMPIXELS;
    hueOffset = (hueOffset + 2) % 256;
}

// 💡 ブリンク（点滅）+ IMU + MIC
void effectBlink() {
    updateIMUColor();  // 🎮 IMU更新
    updateMicFFT();    // 🎤 マイク更新
    
    // IMU + MIC ベースの色相を使用
    int baseHue = (int)(imuBaseHue * 256);
    
    // 🎤 ビート検出で強制点灯
    bool shouldLight = blinkState || beatDetected > 0.5f;
    
    if (shouldLight) {
        for (int i = 0; i < NUMPIXELS; i++) {
            float pos = (float)i / NUMPIXELS;
            // 🎤 位置によって周波数帯域の影響を変える
            float localBrightness = imuBrightness;
            if (pos < 0.33f) {
                localBrightness *= (0.5f + bassLevel * 0.5f);
            } else if (pos < 0.66f) {
                localBrightness *= (0.5f + midLevel * 0.5f);
            } else {
                localBrightness *= (0.5f + highLevel * 0.5f);
            }
            pixels.setPixelColor(i, hsvToColor((baseHue + i * 3) % 256, imuSaturation, localBrightness));
        }
    } else {
        pixels.clear();
    }
    blinkState = !blinkState;
    hueOffset = (hueOffset + 20) % 256;
}

// 🌈 レインボー（虹色グラデーション）+ IMU + MIC
void effectRainbow() {
    updateIMUColor();  // 🎮 IMU更新
    updateMicFFT();    // 🎤 マイク更新
    
    // IMUベースの色相オフセットを使用
    int baseHue = (int)(imuBaseHue * 256);
    
    // 🎤 音量で回転速度が変化
    int rotationSpeed = 2 + (int)(overallVolume * 8);
    
    for (int i = 0; i < NUMPIXELS; i++) {
        int hue = (baseHue + i * 256 / NUMPIXELS + hueOffset) % 256;
        float brightness = imuBrightness;
        
        // 🎤 周波数帯域で明るさ変化
        float pos = (float)i / NUMPIXELS;
        if (pos < 0.33f) {
            brightness *= (0.6f + bassLevel * 0.4f);
        } else if (pos < 0.66f) {
            brightness *= (0.6f + midLevel * 0.4f);
        } else {
            brightness *= (0.6f + highLevel * 0.4f);
        }
        
        // 🥁 ビート時に白っぽく
        float sat = imuSaturation;
        if (beatDetected > 0.3f) {
            sat = max(0.3f, sat - beatDetected * 0.3f);
            brightness = min(1.0f, brightness + beatDetected * 0.3f);
        }
        
        pixels.setPixelColor(i, hsvToColor(hue, sat, brightness));
    }
    hueOffset = (hueOffset + rotationSpeed) % 256;
}

// ✨ スパークル（キラキラ）+ IMU + MIC
void effectSparkle() {
    updateIMUColor();  // 🎮 IMU更新
    updateMicFFT();    // 🎤 マイク更新
    
    // IMUベースの色相を使用
    int baseHue = (int)(imuBaseHue * 256);
    
    // 徐々に暗くする
    float fadeRate = 0.85f * (0.7f + imuBrightness * 0.3f);
    for (int i = 0; i < NUMPIXELS; i++) {
        uint32_t c = pixels.getPixelColor(i);
        int r = ((c >> 16) & 0xFF) * fadeRate;
        int g = ((c >> 8) & 0xFF) * fadeRate;
        int b = (c & 0xFF) * fadeRate;
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
    
    // 🎤 音量でキラキラの量が変化
    int sparkleCount = 3 + (int)(overallVolume * 10);  // 3～13
    
    // ランダムにキラッと光らせる（IMU + MICカラーで）
    for (int i = 0; i < sparkleCount; i++) {
        if (random(100) < 40 + (int)(overallVolume * 30)) {
            int pos = random(NUMPIXELS);
            // 🎤 周波数帯域で色分け
            int hueOffset = 0;
            if (bassLevel > midLevel && bassLevel > highLevel) {
                hueOffset = 0;    // 低音=赤系
            } else if (midLevel > highLevel) {
                hueOffset = 85;   // 中音=緑系
            } else {
                hueOffset = 170;  // 高音=青系
            }
            pixels.setPixelColor(pos, hsvToColor((baseHue + hueOffset + random(30)) % 256, imuSaturation * 0.5f, 1.0));
        }
    }
    
    // 🥁 ビート時に全体フラッシュ
    if (beatDetected > 0.5f) {
        for (int i = 0; i < NUMPIXELS; i += 3) {
            pixels.setPixelColor(i, hsvToColor(baseHue, 0.3f, beatDetected));
        }
    }
}

// 💨 ブリーズ（呼吸）+ IMU + MIC
void effectBreathe() {
    updateIMUColor();  // 🎮 IMU更新
    updateMicFFT();    // 🎤 マイク更新
    
    // IMUベースの色相を使用
    int baseHue = (int)(imuBaseHue * 256);
    
    // 🎤 音量で呼吸速度が変化
    float breathSpeed = 0.03f + overallVolume * 0.05f;
    
    breathValue += breathDir * breathSpeed / 0.03f;
    if (breathValue >= 1.0) {
        breathValue = 1.0;
        breathDir = -0.03;
    } else if (breathValue <= 0.05) {
        breathValue = 0.05;
        breathDir = 0.03;
        hueOffset = (hueOffset + 30) % 256;
    }
    
    // 🥁 ビート時は呼吸をリセットして最大に
    if (beatDetected > 0.5f) {
        breathValue = 1.0f;
        breathDir = -0.03;
    }
    
    // 呼吸の明るさにIMUの明るさも合成
    float finalBrightness = breathValue * imuBrightness;
    
    for (int i = 0; i < NUMPIXELS; i++) {
        // 🎤 位置によって周波数帯域の色を混ぜる
        float pos = (float)i / NUMPIXELS;
        int hue = (baseHue + hueOffset) % 256;
        if (pos < 0.33f && bassLevel > 0.3f) {
            hue = (hue + 0) % 256;    // 低音=赤寄り
        } else if (pos > 0.66f && highLevel > 0.3f) {
            hue = (hue + 170) % 256;  // 高音=青寄り
        }
        pixels.setPixelColor(i, hsvToColor(hue, imuSaturation, finalBrightness));
    }
}

// 🎉 パーティー + IMU + MIC
void effectParty() {
    updateIMUColor();  // 🎮 IMU更新
    updateMicFFT();    // 🎤 マイク更新
    
    // IMUベースの色相を使用
    int baseHue = (int)(imuBaseHue * 256);
    
    // 🎤 音量で点灯確率が変化
    int lightChance = 20 + (int)(overallVolume * 40);  // 20～60%
    
    for (int i = 0; i < NUMPIXELS; i++) {
        if (random(100) < lightChance) {
            // 🎤 周波数帯域で色を決定
            int hue;
            float pos = (float)i / NUMPIXELS;
            if (pos < 0.33f) {
                hue = (baseHue + (int)(bassLevel * 60)) % 256;      // 低音域
            } else if (pos < 0.66f) {
                hue = (baseHue + 85 + (int)(midLevel * 60)) % 256;  // 中音域
            } else {
                hue = (baseHue + 170 + (int)(highLevel * 60)) % 256; // 高音域
            }
            
            float brightness = imuBrightness * (0.7f + overallVolume * 0.3f);
            pixels.setPixelColor(i, hsvToColor(hue, imuSaturation, brightness));
        } else {
            uint32_t c = pixels.getPixelColor(i);
            int r = ((c >> 16) & 0xFF) * 0.7;
            int g = ((c >> 8) & 0xFF) * 0.7;
            int b = (c & 0xFF) * 0.7;
            pixels.setPixelColor(i, pixels.Color(r, g, b));
        }
    }
    
    // 🥁 ビート時に全LEDフラッシュ
    if (beatDetected > 0.6f) {
        for (int i = 0; i < NUMPIXELS; i++) {
            pixels.setPixelColor(i, hsvToColor(baseHue, 0.2f, beatDetected));
        }
    }
}

// 🎮 IMUセンサーエフェクト（オーロラ風）+ MIC
static float auroraOffset = 0;
void effectIMU() {
    // IMU + MIC データ更新
    updateIMUColor();
    updateMicFFT();  // 🎤 マイク更新
    
    // 🎤 音量でオーロラの速度が変化
    float auroraSpeed = 0.05f + overallVolume * 0.15f;
    auroraOffset += auroraSpeed;
    
    for (int i = 0; i < NUMPIXELS; i++) {
        // 2つの波を重ねてゆらぎを表現
        float wave1 = sin(auroraOffset + (float)i * 0.15f) * 0.5f + 0.5f;
        float wave2 = sin(auroraOffset * 0.7f + (float)i * 0.1f) * 0.5f + 0.5f;
        
        float brightness = (wave1 + wave2) * 0.5f;
        
        // 🎤 周波数帯域で明るさを変調
        float pos = (float)i / NUMPIXELS;
        if (pos < 0.33f) {
            brightness *= (0.7f + bassLevel * 0.5f);
        } else if (pos < 0.66f) {
            brightness *= (0.7f + midLevel * 0.5f);
        } else {
            brightness *= (0.7f + highLevel * 0.5f);
        }
        
        uint8_t r, g, b;
        getIMUColor(i, brightness, &r, &g, &b);
        
        // オーロラっぽい緑を少し追加 🌌
        g = min(255, (int)(g + wave2 * 30));
        
        // 🥁 ビート時にフラッシュ
        if (beatDetected > 0.4f && i % 2 == 0) {
            r = min(255, (int)(r + beatDetected * 100));
            g = min(255, (int)(g + beatDetected * 100));
            b = min(255, (int)(b + beatDetected * 100));
        }
        
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
}

// 🎤 マイクからサンプリングしてFFT分析
void updateMicFFT() {
    if (!micReady) {
        // マイクが準備できていない場合はダミーデータ
        bassLevel = 0.1f;
        midLevel = 0.1f;
        highLevel = 0.1f;
        return;
    }
    
    // M5CoreS3の内蔵マイク (ES7210) から取得
    // record()はブロッキング呼び出し
    if (!M5.Mic.record(micBuffer, SAMPLES, SAMPLING_FREQ, false)) {
        // 読み取り失敗時はスキップ
        return;
    }
    
    // 録音完了を待つ（タイムアウト付き）
    uint32_t waitStart = millis();
    while (M5.Mic.isRecording()) {
        if (millis() - waitStart > 100) {  // 100msタイムアウト
            Serial.println("⚠️ Mic timeout");
            return;
        }
        delay(1);
    }
    
    // FFT用データに変換
    for (int i = 0; i < SAMPLES; i++) {
        vReal[i] = (float)micBuffer[i];
        vImag[i] = 0;
    }
        
    // FFT実行 🎵
    FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
    FFT.compute(FFTDirection::Forward);
    FFT.complexToMagnitude();
    
    // 周波数帯域ごとの強度を計算
    // 低音域: 60-250Hz (ベース、キック)
    // 中音域: 250-2000Hz (ボーカル、ギター)
    // 高音域: 2000-4000Hz (ハイハット)
    
    float binWidth = (float)SAMPLING_FREQ / SAMPLES;  // 約62.5Hz/bin
    
    float bassSum = 0, midSum = 0, highSum = 0;
    int bassCount = 0, midCount = 0, highCount = 0;
    
    for (int i = 2; i < SAMPLES / 2; i++) {  // DC成分をスキップ
        float freq = i * binWidth;
        float magnitude = vReal[i];
        
        if (freq >= 60 && freq < 250) {
            bassSum += magnitude;
            bassCount++;
        } else if (freq >= 250 && freq < 2000) {
            midSum += magnitude;
            midCount++;
        } else if (freq >= 2000 && freq < 4000) {
            highSum += magnitude;
            highCount++;
        }
    }
    
    // 平均値を計算して正規化 (0~1)
    float newBass = bassCount > 0 ? bassSum / bassCount / 5000.0f : 0;
    float newMid = midCount > 0 ? midSum / midCount / 4000.0f : 0;
    float newHigh = highCount > 0 ? highSum / highCount / 3000.0f : 0;
    
    // スムージング（急な変化を抑える）
    bassLevel = bassLevel * 0.7f + constrain(newBass, 0, 1) * 0.3f;
    midLevel = midLevel * 0.7f + constrain(newMid, 0, 1) * 0.3f;
    highLevel = highLevel * 0.7f + constrain(newHigh, 0, 1) * 0.3f;
    
    // 全体音量
    overallVolume = (bassLevel + midLevel + highLevel) / 3.0f;
    
    // 🥁 改善されたビート検出アルゴリズム（キック＋スネア対応）
    // 1. 低音域・中音域の履歴を更新
    bassHistory[bassHistoryIndex] = bassLevel;
    midHistory[bassHistoryIndex] = midLevel;
    bassHistoryIndex = (bassHistoryIndex + 1) % BEAT_HISTORY_SIZE;
    
    // 2. 低音域の平均と分散を計算
    float bassHistSum = 0, bassSumSq = 0;
    float midHistSum = 0, midSumSq = 0;
    for (int i = 0; i < BEAT_HISTORY_SIZE; i++) {
        bassHistSum += bassHistory[i];
        bassSumSq += bassHistory[i] * bassHistory[i];
        midHistSum += midHistory[i];
        midSumSq += midHistory[i] * midHistory[i];
    }
    bassAverage = bassHistSum / BEAT_HISTORY_SIZE;
    bassVariance = (bassSumSq / BEAT_HISTORY_SIZE) - (bassAverage * bassAverage);
    midAverage = midHistSum / BEAT_HISTORY_SIZE;
    midVariance = (midSumSq / BEAT_HISTORY_SIZE) - (midAverage * midAverage);
    
    // 3. スネアモード判定（低音が常に高く変化が少ない場合）
    float bassStdDev = sqrt(max(0.0f, bassVariance));
    float midStdDev = sqrt(max(0.0f, midVariance));
    
    // 低音が平均的に高く(>0.35)、かつ変化が少ない(標準偏差<0.06)場合はスネアモード
    useSnareMode = (bassAverage > 0.35f && bassStdDev < 0.06f);
    
    // 4. 適応的閾値を計算（緩め）
    beatThreshold = bassAverage + bassStdDev * 0.8f + 0.01f;
    midThreshold = midAverage + midStdDev * 0.7f + 0.01f;
    
    // 5. ビート検出
    float bassDiff = bassLevel - prevBassLevel;
    float midDiff = midLevel - prevMidLevel;
    uint32_t timeSinceLastBeat = millis() - lastBeatTime;
    
    bool isBeat = false;
    
    if (useSnareMode) {
        // 🥁 スネアモード: 中音域の変化で検出
        if (midLevel > midThreshold && midDiff > 0.015f && timeSinceLastBeat > 120) {
            isBeat = true;
        }
    } else {
        // 🥁 キックモード: 低音域の変化で検出
        if (bassLevel > beatThreshold && bassDiff > 0.015f && timeSinceLastBeat > 120) {
            isBeat = true;
        }
    }
    
    // 追加: 両方のモードで、強めの中音域のスパイクはビートとして検出
    if (!isBeat && midDiff > 0.08f && timeSinceLastBeat > 100) {
        isBeat = true;
    }
    
    if (isBeat) {
        beatDetected = 1.0f;
        
        // BPM計算用にビート時刻を記録
        beatTimes[beatTimeIndex] = millis();
        beatTimeIndex = (beatTimeIndex + 1) % 8;
        beatCount++;
        
        // BPM計算（8回以上のビートがあれば）
        if (beatCount >= 8) {
            uint32_t totalInterval = 0;
            int validIntervals = 0;
            for (int i = 0; i < 7; i++) {
                int idx1 = (beatTimeIndex + i) % 8;
                int idx2 = (beatTimeIndex + i + 1) % 8;
                uint32_t interval = beatTimes[idx2] - beatTimes[idx1];
                // 異常値を除外（150-1500ms = 40-400BPM）
                if (interval > 150 && interval < 1500) {
                    totalInterval += interval;
                    validIntervals++;
                }
            }
            if (validIntervals > 0) {
                float avgInterval = (float)totalInterval / validIntervals;
                bpm = 60000.0f / avgInterval;
            }
        }
        
        lastBeatTime = millis();
        Serial.print(useSnareMode ? "🥁 SNARE! mid:" : "🥁 KICK! bass:");
        Serial.print(useSnareMode ? midLevel : bassLevel, 2);
        Serial.print(" th:");
        Serial.print(useSnareMode ? midThreshold : beatThreshold, 2);
        Serial.print(" BPM:");
        Serial.println((int)bpm);
    } else {
        // フェードアウト
        beatDetected *= 0.85f;
    }
    
    // デバッグ用（時々出力）
    static uint32_t lastDebugTime = 0;
    if (millis() - lastDebugTime > 500) {
        Serial.print(useSnareMode ? "📊 [SNARE] " : "📊 [KICK] ");
        Serial.print("bass:");
        Serial.print(bassLevel, 2);
        Serial.print(" mid:");
        Serial.print(midLevel, 2);
        Serial.print(" bassAvg:");
        Serial.print(bassAverage, 2);
        Serial.print(" bassSD:");
        Serial.println(sqrt(max(0.0f, bassVariance)), 3);
        lastDebugTime = millis();
    }
    
    prevBassLevel = bassLevel;
    prevMidLevel = midLevel;
}

// 🎤 マイクエフェクト（スペクトラム風）
static float micHueOffset = 0;
void effectMic() {
    updateIMUColor();  // IMUも更新
    updateMicFFT();    // マイク＋FFT更新
    
    micHueOffset += 0.5f + overallVolume * 2.0f;  // 音量で回転速度変化
    
    // LEDを周波数帯域で色分け
    // 低音（赤系）→ 中音（緑系）→ 高音（青系）
    
    for (int i = 0; i < NUMPIXELS; i++) {
        float pos = (float)i / NUMPIXELS;
        
        // 位置に応じて周波数帯域の影響を変える
        float bassInfluence, midInfluence, highInfluence;
        
        if (pos < 0.33f) {
            // 下部: 低音域メイン
            bassInfluence = 1.0f;
            midInfluence = 0.3f;
            highInfluence = 0.1f;
        } else if (pos < 0.66f) {
            // 中部: 中音域メイン
            bassInfluence = 0.3f;
            midInfluence = 1.0f;
            highInfluence = 0.3f;
        } else {
            // 上部: 高音域メイン
            bassInfluence = 0.1f;
            midInfluence = 0.3f;
            highInfluence = 1.0f;
        }
        
        // 各帯域の強度を合成
        float localLevel = bassLevel * bassInfluence + 
                          midLevel * midInfluence + 
                          highLevel * highInfluence;
        localLevel = constrain(localLevel, 0, 1);
        
        // 色相: 低音=赤(0), 中音=緑(85), 高音=青(170)
        int hue;
        if (bassInfluence > midInfluence && bassInfluence > highInfluence) {
            hue = (int)(micHueOffset + 0) % 256;    // 赤系
        } else if (midInfluence > highInfluence) {
            hue = (int)(micHueOffset + 85) % 256;   // 緑系
        } else {
            hue = (int)(micHueOffset + 170) % 256;  // 青系
        }
        
        // IMUの影響も加える
        hue = (hue + (int)(imuBaseHue * 50)) % 256;
        
        // ビート検出時は白くフラッシュ
        float brightness = localLevel * 0.8f + 0.1f;
        float saturation = imuSaturation;
        
        if (beatDetected > 0.3f && pos < 0.5f) {
            // ビート時、下半分がフラッシュ
            brightness = min(1.0f, brightness + beatDetected * 0.5f);
            saturation = max(0.3f, saturation - beatDetected * 0.3f);
        }
        
        brightness = brightness * imuBrightness;
        pixels.setPixelColor(i, hsvToColor(hue, saturation, constrain(brightness, 0, 1)));
    }
}

void setup()
{
    M5.begin();
    M5.Power.begin();
    Serial.begin(115200);
    
    Serial.println("\n🐱 NECO LED Party Mode!");
    Serial.println("========================");

    randomSeed(analogRead(0));
    
    // 🎮 IMU初期化 (BMI270 + BMM150) - 内部I2C (SDA=G12, SCL=G11)
    Serial.println("🎮 Initializing IMU (BMI270+BMM150)...");
    if (M5.Imu.begin()) {
        imuReady = true;
        Serial.println("✅ IMU ready! (BMI270 @ 0x69, BMM150 @ 0x10)");
    } else {
        imuReady = false;
        Serial.println("⚠️ IMU init failed - IMU mode will use fallback colors");
    }
    
    // 🎤 マイク初期化 (ES7210 オーディオコーデック)
    Serial.println("🎤 Initializing Microphone (ES7210)...");
    auto mic_cfg = M5.Mic.config();
    mic_cfg.sample_rate = SAMPLING_FREQ;
    mic_cfg.dma_buf_count = 4;
    mic_cfg.dma_buf_len = 256;
    M5.Mic.config(mic_cfg);
    
    if (M5.Mic.begin()) {
        micReady = true;
        Serial.println("✅ Microphone ready! (ES7210 @ 0x40)");
        Serial.print("   Sample Rate: ");
        Serial.print(SAMPLING_FREQ);
        Serial.println(" Hz");
    } else {
        micReady = false;
        Serial.println("⚠️ Microphone init failed - MIC mode will use fallback");
    }
    
    // 猫耳LED初期化 🐱
    pixels.setBrightness(20);  // 🔥 LED焼損防止のため20に変更
    pixels.begin();
    pixels.clear();
    pixels.show();
    Serial.println("✅ NECO OK!");
    
    // 画面初期化 📺
    M5.Lcd.fillScreen(BLACK);
    drawTitle();
    drawButtons();
    
    // 🔄 自動切り替えタイマー初期化
    lastModeChangeTime = millis();
    
    Serial.println("✨ Touch buttons to change mode!");
    Serial.print("🔄 Auto mode change every ");
    Serial.print(modeChangeInterval / 1000);
    Serial.println(" seconds!");
}

// 🎤 デバッグ表示用タイマー
uint32_t lastDebugUpdate = 0;
const uint32_t DEBUG_UPDATE_INTERVAL = 100;  // 100msごとに更新

void loop()
{
    // タッチチェック
    checkTouch();
    
    // 🔄 周期的な自動モード切り替え
    checkAutoModeChange();
    
    // 🎤 デバッグ表示更新（全モード対応）
    if (millis() - lastDebugUpdate >= DEBUG_UPDATE_INTERVAL) {
        lastDebugUpdate = millis();
        drawMicDebug();
        
        // シリアルにも詳細出力
        Serial.print("🎤 Bass:");
        Serial.print((int)(bassLevel * 100));
        Serial.print("% Mid:");
        Serial.print((int)(midLevel * 100));
        Serial.print("% High:");
        Serial.print((int)(highLevel * 100));
        Serial.print("% Beat:");
        Serial.print(beatDetected, 2);
        Serial.println();
    }
    
    // アニメーション更新
    if (millis() - lastUpdateTime >= speeds[currentMode]) {
        lastUpdateTime = millis();
        
        switch (currentMode) {
            case MODE_CHASE:   effectChase();   break;
            case MODE_BLINK:   effectBlink();   break;
            case MODE_RAINBOW: effectRainbow(); break;
            case MODE_SPARKLE: effectSparkle(); break;
            case MODE_BREATHE: effectBreathe(); break;
            case MODE_PARTY:   effectParty();   break;
            case MODE_IMU:     effectIMU();     break;
            case MODE_MIC:     effectMic();     break;
            default: break;
        }
        
        pixels.show();
    }
}
