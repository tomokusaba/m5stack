/*
 * ============================================================================
 * Cat Ear LED Multi-Mode Sketch for M5Stack CoreS3
 * Touch buttons to switch fun effects!
 * ============================================================================
 * 
 * [Hardware Configuration]
 * - M5Stack CoreS3
 *   CPU: ESP32-S3 (Dual-core Xtensa LX7, 240MHz)
 *   Flash: 16MB, PSRAM: 8MB
 *   Display: 2.0" IPS LCD (320x240), Touch screen
 * 
 * [Sensor List]
 * - 6-axis IMU: BMI270 (0x69) - Accelerometer/Gyroscope
 * - Magnetometer: BMM150 (0x10) - Compass
 * - Audio Codec: ES7210 (0x40) - Dual microphone input
 * 
 * [Pin Configuration]
 * - PORT.A (Red): GPIO 2 = Unit Neco (WS2812C-2020 x 70 LEDs)
 * - Internal I2C: GPIO 12 (SDA), GPIO 11 (SCL)
 * - Internal Mic: ES7210 via I2S
 * - Internal Speaker: NS4168 amp via I2S
 * 
 * [Unit Neco Details]
 * - LED: WS2812C-2020 x 70 (35 left ear + 35 right ear)
 * - Voltage: 5V (from M5Stack PORT.A)
 * - Data Pin: GPIO 2
 * 
 * [IMU to LED Control Mapping]
 * - Magnetometer (Direction) -> Hue: N=Red, E=Green, S=Blue, W=Purple
 * - Gyroscope (Rotation) -> Brightness: Rotation speed changes brightness
 * - Accelerometer (Tilt) -> Saturation: Tilt changes color vividness
 * 
 * [Microphone to LED Control Mapping (FFT Analysis)]
 * - Low freq (60-250Hz) -> Red LEDs, Beat detection (Kick)
 * - Mid freq (250-2000Hz) -> Green LEDs, Beat detection (Snare)
 * - High freq (2000-4000Hz) -> Blue LEDs
 * - FFT: 128 samples, 10kHz sampling
 * 
 * [Libraries]
 * - M5CoreS3.h (M5Stack official)
 * - Adafruit_NeoPixel.h (LED control)
 * - arduinoFFT.h (Audio frequency analysis)
 * 
 * ============================================================================
 */

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
    MODE_POV,        // 📝 POV残像テキスト表示
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
// 3列 x 3行、各ボタン 100x32 🐱
Button buttons[] = {
    {  5, 125, 100, 32, 0x07E0, "CHASE",   MODE_CHASE},    // 緑 🌊
    {110, 125, 100, 32, 0xFFE0, "BLINK",   MODE_BLINK},    // 黄 💡
    {215, 125, 100, 32, 0xF81F, "RAINBW",  MODE_RAINBOW},  // マゼンタ 🌈
    {  5, 162, 100, 32, 0x07FF, "SPARKL",  MODE_SPARKLE},  // シアン ✨
    {110, 162, 100, 32, 0xFD20, "BREATH",  MODE_BREATHE},  // オレンジ 💨
    {215, 162, 100, 32, 0xF800, "PARTY",   MODE_PARTY},    // 赤 🎉
    {  5, 199, 100, 32, 0x001F, "IMU",     MODE_IMU},      // 🎮 青
    {110, 199, 100, 32, 0xFC00, "MIC",     MODE_MIC},      // 🎤 黄緑
    {215, 199, 100, 32, 0xFBE0, "POV",     MODE_POV},      // 📝 残像
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
#define SAMPLES 256              // FFTサンプル数（256で周波数分解能39Hz/bin）
#define SAMPLING_FREQ 10000      // サンプリング周波数 (Hz) - ビート検出精度向上のため10kHzに

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

// 🥁 スペクトラルフラックス用（デバッグ表示用にグローバル）
float gKickFlux = 0;             // キックフラックス
float gSnareFlux = 0;            // スネアフラックス
float gKickThresh = 0;           // キック閾値
float gSnareThresh = 0;          // スネア閾値
bool useSnareMode = false;       // スネアモード
int beatCount = 0;               // ビートカウント
float bpm = 0;                   // 推定BPM
uint32_t beatTimes[8];           // 最近8回のビート時刻
int beatTimeIndex = 0;           // ビート時刻インデックス

// 🎼 テンポトラッキング（フェーズロック型4分音符追跡）
float estimatedInterval = 500.0f;   // 推定4分音符間隔(ms) 初期値=120BPM
uint32_t lastConfirmedBeat = 0;     // 最後に確認された4分音符の時刻
float tempoConfidence = 0.0f;       // テンポ推定の信頼度 (0~1)
bool tempoLocked = false;           // テンポがロックされているか
int consecutiveOnBeat = 0;          // 連続して4分音符位置でビートが来た回数
int missedBeats = 0;                // 予測位置にビートが来なかった回数
int consecutiveOffBeat = 0;         // 連続して予測外の位置にビートが来た回数 🔄
#define TEMPO_LOCK_THRESHOLD 4      // テンポロックに必要な連続ビート数（4拍でロック）
#define PHASE_TOLERANCE 0.18f       // 4分音符位相許容範囲 (±18%)
#define EIGHTH_NOTE_ZONE_LO 0.40f   // 8分音符ゾーン下限 (phase=0.5付近)
#define EIGHTH_NOTE_ZONE_HI 0.60f   // 8分音符ゾーン上限
#define SIXTEENTH_NOTE_ZONE_LO 0.15f // 16分音符ゾーン下限 (※4分音符ゾーンの直後から)
#define SIXTEENTH_NOTE_ZONE_HI 0.35f // 16分音符ゾーン上限 (※8分音符ゾーンの直前まで)
#define TEMPO_SMOOTH_FACTOR 0.15f   // テンポ更新の追従速度（小さい=安定）
#define MAX_TEMPO_CHANGE 0.08f      // 1回のテンポ更新の最大変化率 (8%)
#define OFF_BEAT_RESET_COUNT 2      // 連続で予測外ビートがこの回数来たら即リセット

// 🎯 仮想ビート（予測タイミングでLEDを光らせる）
uint32_t lastVirtualBeatTime = 0;    // 最後の仮想ビート発火時刻
int virtualBeatPhase = 0;           // 仮想ビートの拍数カウンタ

// 🥁 リズムパターン認識
enum RhythmPattern {
    PATTERN_UNKNOWN = 0,
    PATTERN_8BEAT,        // 8ビート: K..S..K..S (rock/pop/J-pop)
    PATTERN_FOUR_FLOOR,   // 4つ打ち: KKKK + clap on 2,4 (EDM/dance)
    PATTERN_16BEAT,       // 16ビート: syncopated K (funk/R&B)
    PATTERN_HALFTIME,     // ハーフタイム: K.....S..... (trap/ballad)
    PATTERN_COUNT
};
float kickSlots[8] = {0};           // キックヒストグラム (小節内8分音符解像度)
float snareSlots[8] = {0};          // スネアヒストグラム
RhythmPattern detectedPattern = PATTERN_UNKNOWN;
float patternScore = 0;             // パターンマッチ信頼度 (0~1)
const char* patternNames[] = {"----", "8BT", "4FL", "16B", "HLF"};
int patternAnalysisCount = 0;       // パターン分析回数
int halfTimeAgreement = 0;          // ハーフタイム連続検出回数

// 直近オンセットタイプ追跡 (テンポ学習時のK-S交互判定用)
#define ONSET_TYPE_HISTORY 8
bool onsetTypeHist[ONSET_TYPE_HISTORY] = {false};
int onsetTypeIdx = 0;
int onsetTypeCount = 0;

// � インターバル候補バッファ（倍テンポ誤検出防止）
#define INTERVAL_BUF_SIZE 8
float intervalBuffer[INTERVAL_BUF_SIZE] = {0};
int intervalBufIdx = 0;
int intervalBufCount = 0;

// �🔊 スピーカー（ビート音）
bool speakerEnabled = true;      // スピーカー有効/無効
int beatSoundType = 0;           // ビート音の種類 (0-3)
uint8_t speakerVolume = 80;      // スピーカー音量 (0-255)

// 速度設定
int speeds[] = {30, 300, 20, 50, 30, 40, 30, 20, 1};  // 各モードの更新間隔（POV追加）

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

// 🔊 ビート音を再生（ノリノリサウンド！）
void playBeatSound(bool isSnare) {
    // 前の音を停止してから新しい音を再生
    M5.Speaker.stop();
    
    if (isSnare) {
        // 🥁 スネア風: 高めの音で短く「タッ」
        M5.Speaker.tone(800, 25);  // 800Hz, 25ms
    } else {
        // 🥁 キック風: 低めの音で「ドン」
        switch (beatSoundType) {
            case 0:  // シンプルキック
                M5.Speaker.tone(100, 30);  // 100Hz, 30ms
                break;
            case 1:  // エレクトロキック
                M5.Speaker.tone(60, 35);   // 60Hz, 35ms
                break;
            case 2:  // ポップキック
                M5.Speaker.tone(150, 25);  // 150Hz, 25ms
                break;
            case 3:  // ハードキック
                M5.Speaker.tone(80, 40);   // 80Hz, 40ms
                break;
        }
    }
}

// 🔊 スピーカー ON/OFF 切り替え
void toggleSpeaker() {
    speakerEnabled = !speakerEnabled;
    if (speakerEnabled) {
        M5.Speaker.tone(1000, 50);  // ON確認音
        delay(100);
        M5.Speaker.tone(1500, 50);
    }
    Serial.print("🔊 Speaker: ");
    Serial.println(speakerEnabled ? "ON" : "OFF");
}

// 🔊 ビート音タイプ切り替え
void cycleBeatSound() {
    beatSoundType = (beatSoundType + 1) % 4;
    // 確認音
    M5.Speaker.tone(500 + beatSoundType * 200, 100);
    Serial.print("🎵 Beat sound type: ");
    Serial.println(beatSoundType);
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
    M5.Lcd.fillRect(0, 0, 320, 120, BLACK);
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
    
    // 🎵 ビート関連モードではBPMをタイトルにも大きく表示
    if (currentMode != MODE_POV) {
        M5.Lcd.setCursor(220, 5);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(tempoLocked ? GREEN : DARKGREY);
        if (bpm > 0) {
            M5.Lcd.print((int)bpm);
            M5.Lcd.setTextSize(1);
            M5.Lcd.print("BPM");
        } else {
            M5.Lcd.print("---");
            M5.Lcd.setTextSize(1);
            M5.Lcd.print("BPM");
        }
        // 🔒 テンポロック状態 + パターン名表示
        M5.Lcd.setCursor(220, 22);
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(tempoLocked ? GREEN : YELLOW);
        M5.Lcd.print(tempoLocked ? "LOCK" : "SEEK");
        // 🥁 リズムパターン名
        if (detectedPattern != PATTERN_UNKNOWN) {
            M5.Lcd.print(" ");
            M5.Lcd.setTextColor(CYAN);
            M5.Lcd.print(patternNames[detectedPattern]);
        }
    }
}

// 🎤 マイクデバッグ情報を描画（全モード対応）
void drawMicDebug() {
    // 🎵 タイトルエリアのBPM表示を更新（100msごとに最新値に）
    if (currentMode != MODE_POV) {
        M5.Lcd.fillRect(220, 5, 100, 28, BLACK);  // BPM+LOCK表示エリアクリア
        M5.Lcd.setCursor(220, 5);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(tempoLocked ? GREEN : DARKGREY);
        if (bpm > 0) {
            M5.Lcd.print((int)bpm);
            M5.Lcd.setTextSize(1);
            M5.Lcd.print("BPM");
        } else {
            M5.Lcd.print("---");
            M5.Lcd.setTextSize(1);
            M5.Lcd.print("BPM");
        }
        // 🔒 テンポロック状態 + パターン名表示
        M5.Lcd.setCursor(220, 22);
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(tempoLocked ? GREEN : YELLOW);
        M5.Lcd.print(tempoLocked ? "LOCK" : "SEEK");
        if (detectedPattern != PATTERN_UNKNOWN) {
            M5.Lcd.print(" ");
            M5.Lcd.setTextColor(CYAN);
            M5.Lcd.print(patternNames[detectedPattern]);
        }
    }
    
    // デバッグエリア（タイトル下、ボタン上）
    M5.Lcd.fillRect(0, 55, 320, 65, BLACK);
    
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
    
    // BPM表示 + テンポロック状態
    M5.Lcd.setCursor(185, 58);
    M5.Lcd.setTextColor(tempoLocked ? GREEN : MAGENTA);
    M5.Lcd.print(tempoLocked ? "\xF0\x9F\x94\x92" : "  ");  // 🔒
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
    int barY = 70;
    int barHeight = 13;
    int barMaxWidth = 200;
    
    // 🔴 低音 (Bass) + 適応閾値マーカー
    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(5, barY + 2);
    M5.Lcd.print("BASS");
    int bassWidth = (int)(bassLevel * barMaxWidth);
    M5.Lcd.fillRect(45, barY, bassWidth, barHeight, RED);
    M5.Lcd.drawRect(45, barY, barMaxWidth, barHeight, DARKGREY);
    
    // スペクトラルフラックス閾値マーカー（黄色の線）
    int thresholdX = 45 + (int)(gKickThresh * 10 * barMaxWidth);  // スケール調整
    if (thresholdX > 45 && thresholdX < 45 + barMaxWidth) {
        M5.Lcd.drawFastVLine(thresholdX, barY, barHeight, YELLOW);
        M5.Lcd.drawFastVLine(thresholdX + 1, barY, barHeight, YELLOW);
    }
    
    M5.Lcd.setCursor(250, barY + 2);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.print((int)(bassLevel * 100));
    M5.Lcd.print("%");
    
    // 🟢 中音 (Mid)
    barY += barHeight + 3;
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
    barY += barHeight + 3;
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

// Mode switch (common reset process)
void changeMode(Mode newMode) {
    // POVモード: 明るさ＋LED数 調整 🔆⚡
    if (newMode == MODE_POV && currentMode != MODE_POV) {
        pixels.setBrightness(255);  // 最大輝度（瞬間点灯なので安全）
        pixels.updateLength(53);    // LED 0-52 のみ転送 → ~1590μs（25%高速化!）🏎️
    } else if (newMode != MODE_POV && currentMode == MODE_POV) {
        pixels.setBrightness(20);   // 通常に戻す
        pixels.updateLength(70);    // 全LED復帰
    }
    currentMode = newMode;
    animPosition = 0;
    hueOffset = 0;
    breathValue = 0;
    breathDir = 0.05;
    
    // Note: Beat detection variables (static in updateMicFFT) are NOT reset
    // This allows continuous beat detection across mode changes
    
    Serial.print("Mode changed: ");
    Serial.println(buttons[currentMode].label);
    
    drawTitle();
    drawButtons();
}

// 🎲 ランダムモード切り替え（残像モードは除外 📝）
void randomModeChange() {
    Mode newMode;
    int attempts = 0;
    do {
        newMode = (Mode)random(MODE_COUNT);
        attempts++;
    } while ((newMode == currentMode || newMode == MODE_POV) && attempts < 20);
    
    if (newMode == MODE_POV) return;  // 安全弁: それでもPOVならスキップ
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

// Chase effect + IMU + MIC
void effectChase() {
    updateIMUColor();
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
        
        // 🥁 ビート時に全体フラッシュ！
        if (beatDetected > 0.2f) {
            brightness = min(1.0f, brightness + beatDetected * 0.8f);
        }
        
        uint32_t color = hsvToColor((baseHue + pos * 4 + hueOffset) % 256, imuSaturation, brightness);
        pixels.setPixelColor(pos, color);
    }
    
    // 💥 ビート時: 全LEDを白くフラッシュ
    if (beatDetected > 0.5f) {
        for (int i = 0; i < NUMPIXELS; i++) {
            pixels.setPixelColor(i, hsvToColor(baseHue, 0.15f, beatDetected));
        }
    }
    
    animPosition = (animPosition + speed) % NUMPIXELS;
    hueOffset = (hueOffset + 2) % 256;
}

// Blink effect + IMU + MIC
void effectBlink() {
    updateIMUColor();
    
    // IMU + MIC ベースの色相を使用
    int baseHue = (int)(imuBaseHue * 256);
    
    // 🎤 ビート検出で強制点灯
    bool shouldLight = blinkState || beatDetected > 0.2f;
    
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
            
            // 💥 ビート時は最大輝度で白フラッシュ
            float sat = imuSaturation;
            if (beatDetected > 0.3f) {
                localBrightness = min(1.0f, localBrightness + beatDetected * 0.8f);
                sat = max(0.1f, sat - beatDetected * 0.6f);
            }
            pixels.setPixelColor(i, hsvToColor((baseHue + i * 3) % 256, sat, localBrightness));
        }
    } else {
        pixels.clear();
    }
    blinkState = !blinkState;
    hueOffset = (hueOffset + 20) % 256;
}

// Rainbow effect + IMU + MIC
void effectRainbow() {
    updateIMUColor();
    
    // IMU base hue offset
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
        
        // 🥁 ビート時に白くフラッシュ！
        float sat = imuSaturation;
        if (beatDetected > 0.2f) {
            sat = max(0.1f, sat - beatDetected * 0.7f);
            brightness = min(1.0f, brightness + beatDetected * 0.7f);
        }
        
        pixels.setPixelColor(i, hsvToColor(hue, sat, brightness));
    }
    hueOffset = (hueOffset + rotationSpeed) % 256;
}

// Sparkle effect + IMU + MIC
void effectSparkle() {
    updateIMUColor();
    
    // IMU base hue
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
    
    // 💥 ビート時に全LEDフラッシュ（全点、強め）
    if (beatDetected > 0.3f) {
        for (int i = 0; i < NUMPIXELS; i++) {
            pixels.setPixelColor(i, hsvToColor(baseHue, 0.15f, min(1.0f, beatDetected * 1.2f)));
        }
    }
}

// Breathe effect + IMU + MIC
void effectBreathe() {
    updateIMUColor();
    
    // IMU base hue
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
    
    // 💥 ビート時は呼吸をリセットして最大輝度にジャンプ！
    if (beatDetected > 0.2f) {
        breathValue = min(1.0f, 0.8f + beatDetected * 0.3f);
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

// Party effect + IMU + MIC
void effectParty() {
    updateIMUColor();
    
    // IMU base hue
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
    
    // 💥 ビート時に全LEDフラッシュ（過激！）
    if (beatDetected > 0.3f) {
        for (int i = 0; i < NUMPIXELS; i++) {
            pixels.setPixelColor(i, hsvToColor(baseHue, 0.1f, min(1.0f, beatDetected * 1.3f)));
        }
    }
}

// IMU sensor effect (Aurora style) + MIC
static float auroraOffset = 0;
void effectIMU() {
    updateIMUColor();
    
    // Volume affects aurora speed
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
        
        // 💥 ビート時に全LEDフラッシュ！
        if (beatDetected > 0.2f) {
            int flash = (int)(beatDetected * 200);
            r = min(255, r + flash);
            g = min(255, g + flash);
            b = min(255, b + flash);
        }
        
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
}

// ============================================================
// 🥁 RHYTHM PATTERN RECOGNITION FUNCTIONS
// 代表的リズムパターンとの相関でBPM計算精度を向上
// ============================================================

// パターンテンプレート (8スロット: 拍1,裏1,拍2,裏2,拍3,裏3,拍4,裏4)
// slot: 0=beat1, 1=&of1, 2=beat2, 3=&of2, 4=beat3, 5=&of3, 6=beat4, 7=&of4
static const float TMPL_KICK_8BEAT[]   = {1,0,0,0, 1,0,0,0};  // K on 1,3
static const float TMPL_SNARE_8BEAT[]  = {0,0,1,0, 0,0,1,0};  // S on 2,4
static const float TMPL_KICK_4FLOOR[]  = {1,0,1,0, 1,0,1,0};  // K on every beat
static const float TMPL_SNARE_4FLOOR[] = {0,0,1,0, 0,0,1,0};  // S/clap on 2,4
static const float TMPL_KICK_16BEAT[]  = {1,0,0,1, 1,0,0,0};  // K syncopated
static const float TMPL_SNARE_16BEAT[] = {0,0,1,0, 0,0,1,0};  // S on 2,4
static const float TMPL_KICK_HALF[]    = {1,0,0,0, 0,0,0,0};  // K on 1 only
static const float TMPL_SNARE_HALF[]   = {0,0,0,0, 1,0,0,0};  // S on 3 only

// コサイン類似度でパターンマッチング
float patternCorrelation(const float* hist, const float* tmpl) {
    float dot = 0, normH = 0, normT = 0;
    for (int i = 0; i < 8; i++) {
        dot += hist[i] * tmpl[i];
        normH += hist[i] * hist[i];
        normT += tmpl[i] * tmpl[i];
    }
    if (normH < 0.001f || normT < 0.001f) return 0;
    return dot / (sqrtf(normH) * sqrtf(normT));
}

// オンセットを小節内ヒストグラムに記録
void recordOnsetToPattern(bool isKick, float phase, int currentBeatCount) {
    int quarterBeat = currentBeatCount % 4;  // 0-3: 小節内の拍位置
    int slot = quarterBeat * 2;              // 0,2,4,6 = 表拍
    if (phase > 0.3f && phase < 0.7f) {
        slot += 1;  // 8分音符位置 → 奇数スロット (1,3,5,7)
    }
    if (slot >= 0 && slot < 8) {
        if (isKick) kickSlots[slot] += 1.0f;
        else snareSlots[slot] += 1.0f;
    }
}

// ヒストグラム減衰（毎フレーム呼ぶ）
void decayPatternHistogram() {
    for (int i = 0; i < 8; i++) {
        kickSlots[i] *= 0.985f;
        snareSlots[i] *= 0.985f;
    }
}

// 🥁 パターン分析: 4つの回転を試して最良マッチを探す
void analyzeRhythmPattern() {
    const float* kickTmpls[]  = {TMPL_KICK_8BEAT, TMPL_KICK_4FLOOR, TMPL_KICK_16BEAT, TMPL_KICK_HALF};
    const float* snareTmpls[] = {TMPL_SNARE_8BEAT, TMPL_SNARE_4FLOOR, TMPL_SNARE_16BEAT, TMPL_SNARE_HALF};
    
    float bestScore = 0;
    int bestPattern = 0;
    float rotatedKick[8], rotatedSnare[8];
    
    // 4つの回転（拍の開始位置を0-3でずらす）を全試行
    for (int rot = 0; rot < 4; rot++) {
        for (int i = 0; i < 8; i++) {
            int src = (i + rot * 2) % 8;
            rotatedKick[i] = kickSlots[src];
            rotatedSnare[i] = snareSlots[src];
        }
        
        for (int p = 0; p < 4; p++) {
            float kCorr = patternCorrelation(rotatedKick, kickTmpls[p]);
            float sCorr = patternCorrelation(rotatedSnare, snareTmpls[p]);
            float combined = kCorr * 0.5f + sCorr * 0.5f;
            
            if (combined > bestScore) {
                bestScore = combined;
                bestPattern = p + 1;  // +1: PATTERN_UNKNOWN=0をスキップ
            }
        }
    }
    
    if (bestScore > 0.3f) {
        detectedPattern = (RhythmPattern)bestPattern;
        patternScore = bestScore;
    } else {
        detectedPattern = PATTERN_UNKNOWN;
        patternScore = bestScore;
    }
    patternAnalysisCount++;
}

// K-S交互出現の検出（テンポ学習時に8分音符誤検出を防ぐ）
// 8ビートではKick→Snare→Kick→Snareと交互に来る
bool isKickSnareAlternating() {
    if (onsetTypeCount < 3) return false;
    int alternateCount = 0;
    int n = min(onsetTypeCount, ONSET_TYPE_HISTORY);
    for (int i = 1; i < n; i++) {
        int prev = (onsetTypeIdx - i - 1 + ONSET_TYPE_HISTORY) % ONSET_TYPE_HISTORY;
        int curr = (onsetTypeIdx - i + ONSET_TYPE_HISTORY) % ONSET_TYPE_HISTORY;
        if (onsetTypeHist[prev] != onsetTypeHist[curr]) alternateCount++;
    }
    return (float)alternateCount / (n - 1) > 0.6f;
}

// オンセットタイプを記録
void recordOnsetType(bool isKick) {
    onsetTypeHist[onsetTypeIdx] = isKick;
    onsetTypeIdx = (onsetTypeIdx + 1) % ONSET_TYPE_HISTORY;
    onsetTypeCount = min(onsetTypeCount + 1, ONSET_TYPE_HISTORY);
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
    
    // DC offset removal (important for beat detection!)
    int32_t dcOffset = 0;
    for (int i = 0; i < SAMPLES; i++) {
        dcOffset += micBuffer[i];
    }
    dcOffset /= SAMPLES;
    
    // FFT data with DC removal and windowing prep
    for (int i = 0; i < SAMPLES; i++) {
        vReal[i] = (float)(micBuffer[i] - dcOffset);
        vImag[i] = 0;
    }
        
    // FFT execution
    FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
    FFT.compute(FFTDirection::Forward);
    FFT.complexToMagnitude();
    
    // Frequency resolution: 10000Hz / 256 = 39.1Hz/bin
    float binWidth = (float)SAMPLING_FREQ / SAMPLES;
    
    // ============================================================
    // IMPROVED BEAT DETECTION - Energy-based with Sub-band Analysis
    // ============================================================
    
    // Sub-band energy calculation (more precise frequency ranges)
    // NOTE: Raised low cutoff from 40Hz to 80Hz to avoid room noise/hum
    // Kick drum: 80-150Hz - fundamental of bass drum (avoiding sub-bass noise)
    // Snare body: 150-400Hz - body of snare
    // Snare snap: 2000-4000Hz - snare wire brightness
    
    float subBass = 0;    // 80-150Hz  - Kick fundamental (raised from 40Hz)
    float lowMid = 0;     // 150-400Hz - Snare body
    float highMid = 0;    // 2000-4000Hz - Snare snap/brightness
    
    for (int i = 1; i < SAMPLES / 2; i++) {
        float freq = i * binWidth;
        float mag = vReal[i];
        
        // Skip very low frequencies (noise floor: fans, hum, rumble)
        // Raised cutoff to 120Hz to avoid room noise, HVAC, PC fans etc.
        if (freq >= 120 && freq < 200) {
            subBass += mag * mag;  // Use power (squared) for better dynamics
        } else if (freq >= 200 && freq < 500) {
            lowMid += mag * mag;
        } else if (freq >= 2000 && freq < 4000) {
            highMid += mag * mag;
        }
    }
    
    // Convert to RMS-like values (very high divisors to reduce sensitivity)
    subBass = sqrtf(subBass) / 6000.0f;  // Much higher divisor
    lowMid = sqrtf(lowMid) / 5000.0f;
    highMid = sqrtf(highMid) / 2000.0f;
    
    // ============================================================
    // SILENCE DETECTION - Stop beat detection when audio is too quiet
    // ============================================================
    float totalEnergy = subBass + lowMid + highMid;
    static float avgEnergy = 0;
    avgEnergy = avgEnergy * 0.95f + totalEnergy * 0.05f;
    
    // If average energy is below noise floor, skip beat detection
    // Your avgE shows ~1.5 in quiet room, so set threshold to 2.5
    // Only real music/sound should exceed this
    bool isSilent = (avgEnergy < 2.5f);
    
    // ============================================================
    // ONSET DETECTION using first-order difference + half-wave rectification
    // ============================================================
    static float prevSubBass = 0, prevLowMid = 0, prevHighMid = 0;
    
    // Calculate onset (only positive changes = sound starting)
    float kickOnset = max(0.0f, subBass - prevSubBass);
    float snareBodyOnset = max(0.0f, lowMid - prevLowMid);
    float snareSnapOnset = max(0.0f, highMid - prevHighMid);
    
    // Combined snare onset (body + snap)
    float snareOnset = snareBodyOnset * 0.6f + snareSnapOnset * 0.4f;
    
    // Store for next frame
    prevSubBass = subBass;
    prevLowMid = lowMid;
    prevHighMid = highMid;
    
    // If silent, reset onset values to prevent false triggers
    if (isSilent) {
        kickOnset = 0;
        snareOnset = 0;
    }
    
    // ============================================================
    // ADAPTIVE THRESHOLD with exponential moving average
    // ============================================================
    #define ONSET_HISTORY 16
    static float kickOnsetHist[ONSET_HISTORY] = {0};
    static float snareOnsetHist[ONSET_HISTORY] = {0};
    static int onsetIdx = 0;
    
    kickOnsetHist[onsetIdx] = kickOnset;
    snareOnsetHist[onsetIdx] = snareOnset;
    onsetIdx = (onsetIdx + 1) % ONSET_HISTORY;
    
    // Calculate mean and variance for adaptive threshold
    float kickMean = 0, snareMean = 0;
    float kickMax = 0, snareMax = 0;
    for (int i = 0; i < ONSET_HISTORY; i++) {
        kickMean += kickOnsetHist[i];
        snareMean += snareOnsetHist[i];
        if (kickOnsetHist[i] > kickMax) kickMax = kickOnsetHist[i];
        if (snareOnsetHist[i] > snareMax) snareMax = snareOnsetHist[i];
    }
    kickMean /= ONSET_HISTORY;
    snareMean /= ONSET_HISTORY;
    
    // Adaptive threshold: mean + factor * (max - mean)
    // Lower factor = more sensitive detection
    float kickThresh = kickMean + (kickMax - kickMean) * 0.3f + 0.005f;
    float snareThresh = snareMean + (snareMax - snareMean) * 0.3f + 0.005f;
    
    // Minimum threshold floor (noise gate)
    kickThresh = max(kickThresh, 0.04f);
    snareThresh = max(snareThresh, 0.03f);
    
    // Global variables for debug display
    gKickFlux = kickOnset;
    gSnareFlux = snareOnset;
    gKickThresh = kickThresh;
    gSnareThresh = snareThresh;
    
    // ============================================================
    // 🎼 PHASE-LOCKED BEAT DECISION（フェーズロック型4分音符追跡）
    // 音楽的に4分音符の位置を予測し、8分音符と区別する
    // ============================================================
    uint32_t now = millis();
    uint32_t timeSinceLastBeat = now - lastBeatTime;
    bool rawOnset = false;
    
    // --- Step 1: 生のオンセット検出 ---
    if (!isSilent && timeSinceLastBeat > 60) {  // 最小60ms（16分音符@240BPM≈62ms）
        if (kickOnset > kickThresh && kickOnset > snareOnset * 1.2f) {
            rawOnset = true;
            useSnareMode = false;
        } else if (snareOnset > snareThresh) {
            rawOnset = true;
            useSnareMode = true;
        }
        // 🥁 オンセットタイプを記録（リズムパターン分析用）
        if (rawOnset) {
            recordOnsetType(!useSnareMode);  // kick=true, snare=false
        }
    }
    
    // 🥁 パターンヒストグラム減衰（毎フレーム）
    decayPatternHistogram();
    
    // --- Step 2: フェーズ（拍子内の位置）を計算 ---
    // phase = 0.0 が4分音符の位置、0.5 が8分音符の裏拍
    float timeSinceConfirmed = (float)(now - lastConfirmedBeat);
    float phase = 0;
    if (estimatedInterval > 0 && lastConfirmedBeat > 0) {
        phase = fmodf(timeSinceConfirmed, estimatedInterval) / estimatedInterval;
    }
    
    // フェーズを -0.5 ~ +0.5 に正規化（0が拍頭）
    float phaseError = phase;
    if (phaseError > 0.5f) phaseError -= 1.0f;
    
    bool isBeat = false;
    bool isQuarterNote = false;
    
    if (rawOnset) {
        if (!tempoLocked) {
            // --- Step 3a: テンポ未確定 → インターバルを収集して倍テンポを検出 ---
            isBeat = true;
            isQuarterNote = true;  // テンポ未確定時はすべて4分音符候補
            
            if (lastBeatTime > 0) {
                float rawInterval = (float)timeSinceLastBeat;
                
                // インターバルをバッファに記録（BPM範囲: 60～240 BPMの8分音符も含む）
                if (rawInterval >= 125 && rawInterval < 1000) {
                    intervalBuffer[intervalBufIdx] = rawInterval;
                    intervalBufIdx = (intervalBufIdx + 1) % INTERVAL_BUF_SIZE;
                    intervalBufCount = min(intervalBufCount + 1, INTERVAL_BUF_SIZE);
                }
                
                // 妥当な範囲のインターバルのみ学習 (60~240 BPM)
                if (rawInterval > 250 && rawInterval < 1000) {
                    // 8分音符の可能性: インターバルが現在の推定の約半分なら倍にする
                    // ❗ 但しK-S交互パターンなら8ビートの4分音符なので倍にしない
                    if (estimatedInterval > 0 && rawInterval < estimatedInterval * 0.65f && rawInterval > estimatedInterval * 0.35f) {
                        if (isKickSnareAlternating()) {
                            Serial.println("🥁 K-S alternating → this IS the quarter note!");
                        } else {
                            rawInterval *= 2.0f;
                        }
                    }
                    
                    if (beatCount < 2) {
                        estimatedInterval = rawInterval;
                    } else {
                        estimatedInterval = estimatedInterval * (1.0f - TEMPO_SMOOTH_FACTOR) 
                                          + rawInterval * TEMPO_SMOOTH_FACTOR;
                    }
                    consecutiveOnBeat++;
                } else if (rawInterval >= 125 && rawInterval < 250) {
                    // 8分音符として処理、テンポ学習には使わないがロック進捗は維持
                    isQuarterNote = false;
                } else if (rawInterval >= 60 && rawInterval < 125) {
                    // 16分音符として処理
                    isQuarterNote = false;
                }
                
                // 📊 ロック直前: バッファ内インターバルを分析して倍テンポを検出・補正
                if (consecutiveOnBeat >= TEMPO_LOCK_THRESHOLD) {
                    // バッファ内に "estimatedIntervalの約半分" が多数あるか確認
                    int halfCount = 0;
                    int fullCount = 0;
                    int n = min(intervalBufCount, INTERVAL_BUF_SIZE);
                    float halfLo = estimatedInterval * 0.35f;
                    float halfHi = estimatedInterval * 0.65f;
                    float fullLo = estimatedInterval * 0.75f;
                    float fullHi = estimatedInterval * 1.35f;
                    
                    for (int j = 0; j < n; j++) {
                        if (intervalBuffer[j] >= halfLo && intervalBuffer[j] <= halfHi) halfCount++;
                        if (intervalBuffer[j] >= fullLo && intervalBuffer[j] <= fullHi) fullCount++;
                    }
                    
                    // 🔍 半分インターバルが多い = 裏拍を拾っていた！ → テンポを倍に補正
                    if (halfCount >= fullCount && halfCount >= 2) {
                        estimatedInterval *= 2.0f;
                        if (estimatedInterval <= 1000.0f) {
                            Serial.print("🔄 DOUBLE-TEMPO FIX! halfCount:");
                            Serial.print(halfCount);
                            Serial.print(" fullCount:");
                            Serial.print(fullCount);
                            Serial.print(" newInterval:");
                            Serial.println((int)estimatedInterval);
                        } else {
                            estimatedInterval /= 2.0f;  // 範囲外なら戻す
                        }
                    }
                    
                    tempoLocked = true;
                    tempoConfidence = 0.5f;
                    missedBeats = 0;
                    lastVirtualBeatTime = now;
                    virtualBeatPhase = 0;
                    // バッファリセット
                    intervalBufCount = 0;
                    intervalBufIdx = 0;
                    Serial.print("🔒 TEMPO LOCKED! BPM:");
                    Serial.println((int)(60000.0f / estimatedInterval));
                }
            }
        } else {
            // --- Step 3b: テンポ確定済み → フェーズ+インターバル比率で音符種別を判定 ---
            float absPhaseError = fabsf(phaseError);
            
            // 🎵 インターバル比率も並行で計算（フェーズと相互確認用）
            float intervalRatio = (estimatedInterval > 0) ? (float)timeSinceLastBeat / estimatedInterval : 1.0f;
            // ratio ≈ 1.0 → 4分音符, ≈ 0.5 → 8分音符, ≈ 0.25 → 16分音符
            bool intervalSays4th  = (intervalRatio > 0.8f && intervalRatio < 1.3f);
            bool intervalSays8th  = (intervalRatio > 0.4f && intervalRatio < 0.65f);
            bool intervalSays16th = (intervalRatio > 0.18f && intervalRatio < 0.35f);
            
            // --- ゾーン配置（重複なし）---
            // phase: [0.00-0.15] 4分音符 | [0.15-0.35] 16分音符 | [0.35-0.65] 8分音符 | [0.65-0.85] 16分音符 | [0.85-1.00] 4分音符
            
            if (absPhaseError < PHASE_TOLERANCE || (absPhaseError < PHASE_TOLERANCE + 0.05f && intervalSays4th)) {
                // ✅ 4分音符: フェーズが近い、またはフェーズがややズレてもインターバルが確認
                // ✅ 4分音符位置に近い → 確定ビート
                isBeat = true;
                isQuarterNote = true;
                consecutiveOnBeat++;
                consecutiveOffBeat = 0;
                missedBeats = 0;
                // 🥁 パターンヒストグラムに記録
                recordOnsetToPattern(!useSnareMode, phase, beatCount);
                
                // テンポ微調整（位相誤差に基づく緩やかな補正）
                float correction = phaseError * estimatedInterval * TEMPO_SMOOTH_FACTOR;
                float maxDelta = estimatedInterval * MAX_TEMPO_CHANGE;
                correction = constrain(correction, -maxDelta, maxDelta);
                estimatedInterval += correction;
                
                // BPM範囲制限 (60~240 BPM)
                estimatedInterval = constrain(estimatedInterval, 250.0f, 1000.0f);
                tempoConfidence = min(1.0f, tempoConfidence + 0.1f);
                
            } else if ((phase > SIXTEENTH_NOTE_ZONE_LO && phase < SIXTEENTH_NOTE_ZONE_HI)
                    || (phase > (1.0f - SIXTEENTH_NOTE_ZONE_HI) && phase < (1.0f - SIXTEENTH_NOTE_ZONE_LO))) {
                // 🎶 16分音符ゾーン (phase≈0.25 or 0.75)
                // インターバル比率でも確認
                bool confirmed16th = intervalSays16th;
                isBeat = false;
                isQuarterNote = false;
                consecutiveOffBeat = 0;  // 16分音符は「予測内」
                recordOnsetToPattern(!useSnareMode, phase, beatCount);
                Serial.print(confirmed16th ? "♬16th(conf)" : "♬16th(phase)");
                Serial.print(" ph:");
                Serial.print(phase, 2);
                Serial.print(" ratio:");
                Serial.println(intervalRatio, 2);
                
            } else if (phase > EIGHTH_NOTE_ZONE_LO && phase < EIGHTH_NOTE_ZONE_HI) {
                // 🎵 8分音符ゾーン（拍間の中間付近）
                bool confirmed8th = intervalSays8th;
                isBeat = false;
                isQuarterNote = false;
                consecutiveOffBeat = 0;  // 8分音符は「予測内」
                recordOnsetToPattern(!useSnareMode, phase, beatCount);
                Serial.print(confirmed8th ? "♪8th(conf)" : "♪8th(phase)");
                Serial.print(" ph:");
                Serial.print(phase, 2);
                Serial.print(" ratio:");
                Serial.println(intervalRatio, 2);
            } else {
                // ❌ 予測から外れた位置にビート → テンポ変化の可能性
                consecutiveOffBeat++;
                Serial.print("⚠️ OFF-BEAT #");
                Serial.print(consecutiveOffBeat);
                Serial.print(" (phase:");
                Serial.print(phase, 2);
                Serial.println(")");
                
                if (consecutiveOffBeat >= OFF_BEAT_RESET_COUNT) {
                    // 🔄 テンポ変化検出！即座にロック解除して新しいテンポで再学習
                    tempoLocked = false;
                    tempoConfidence = 0;
                    consecutiveOnBeat = 0;
                    consecutiveOffBeat = 0;
                    missedBeats = 0;
                    beatCount = 0;
                    // 🥁 パターン認識もリセット
                    detectedPattern = PATTERN_UNKNOWN;
                    patternScore = 0;
                    halfTimeAgreement = 0;
                    onsetTypeCount = 0;
                    for (int j = 0; j < 8; j++) { kickSlots[j] = 0; snareSlots[j] = 0; }
                    intervalBufCount = 0; intervalBufIdx = 0;  // バッファクリア
                    // 直前のインターバルを新テンポの第一候補として検討
                    float rawInterval = (float)timeSinceLastBeat;
                    if (rawInterval > 250 && rawInterval < 1000) {
                        estimatedInterval = rawInterval;  // 新テンポ候補として一旦採用
                    }
                    Serial.print("🔓 TEMPO RESET! new candidate: ");
                    Serial.print((int)(60000.0f / estimatedInterval));
                    Serial.println(" BPM");
                }
                
                // ロック中のオフビートではLED反応なし
                isBeat = false;
                isQuarterNote = false;
            }
        }
    }
    
    // --- Step 4: ミスビート検出（予測位置にビートが来なかった場合）---
    if (tempoLocked && !rawOnset) {
        // 予測位置を過ぎたかチェック（位相がTOLERANCEを超えた直後）
        float expectedNext = estimatedInterval;
        if (timeSinceConfirmed > expectedNext * (1.0f + PHASE_TOLERANCE) 
            && timeSinceConfirmed < expectedNext * (1.0f + PHASE_TOLERANCE + 0.1f)) {
            missedBeats++;
            if (missedBeats > 4) {
                // テンポを見失った → ロック解除して再学習
                tempoLocked = false;
                tempoConfidence = 0;
                consecutiveOnBeat = 0;
                consecutiveOffBeat = 0;
                missedBeats = 0;
                beatCount = 0;
                // 🥁 パターン認識もリセット
                detectedPattern = PATTERN_UNKNOWN;
                patternScore = 0;
                halfTimeAgreement = 0;
                onsetTypeCount = 0;
                for (int j = 0; j < 8; j++) { kickSlots[j] = 0; snareSlots[j] = 0; }
                intervalBufCount = 0; intervalBufIdx = 0;  // バッファクリア
                Serial.println("🔓 TEMPO UNLOCKED (missed beats)");
            }
        }
    }
    
    // --- Step 5: ビート確定処理 ---
    if (isBeat) {
        beatDetected = isQuarterNote ? 1.0f : 0.6f;  // 8分音符は弱めに反応
        
        // BPM記録（4分音符のみ）
        if (isQuarterNote) {
            beatTimes[beatTimeIndex] = now;
            beatTimeIndex = (beatTimeIndex + 1) % 8;
            beatCount++;
            lastConfirmedBeat = now;
            lastVirtualBeatTime = now;  // 🎯 仮想ビートも同期
            virtualBeatPhase = 0;
        }
        
        // BPM計算（推定インターバルから直接算出）
        if (estimatedInterval > 0) {
            float newBpm = 60000.0f / estimatedInterval;
            if (bpm > 0) {
                bpm = bpm * 0.8f + newBpm * 0.2f;  // BPM表示もスムーズに
            } else {
                bpm = newBpm;
            }
        }
        
        lastBeatTime = now;
        Serial.print(isQuarterNote ? "♩" : "♪");
        Serial.print(useSnareMode ? " SNARE" : " KICK");
        Serial.print(" phase:");
        Serial.print(phase, 2);
        Serial.print(" intv:");
        Serial.print((int)estimatedInterval);
        Serial.print("ms conf:");
        Serial.print(tempoConfidence, 1);
        Serial.print(" BPM:");
        Serial.println((int)bpm);
        
        // 🥁 パターン分析（4拍ごとに実行）
        if (isQuarterNote && beatCount > 0 && beatCount % 4 == 0) {
            analyzeRhythmPattern();
            Serial.print("🥁 Pattern: ");
            Serial.print(patternNames[detectedPattern]);
            Serial.print(" score:");
            Serial.print(patternScore, 2);
            
            // 🔄 ハーフタイム検出によるBPM補正
            // 3回連続ハーフタイム判定 + BPMが高め → テンポ倍速誤検出の可能性
            if (detectedPattern == PATTERN_HALFTIME && patternScore > 0.5f) {
                halfTimeAgreement++;
                if (halfTimeAgreement >= 3 && bpm > 100) {
                    float newInterval = estimatedInterval * 2.0f;
                    if (newInterval <= 1000.0f) {
                        estimatedInterval = newInterval;
                        bpm = 60000.0f / estimatedInterval;
                        // ヒストグラムリセット（再分析のため）
                        for (int j = 0; j < 8; j++) { kickSlots[j] = 0; snareSlots[j] = 0; }
                        halfTimeAgreement = 0;
                        Serial.print(" → HALFTIME CORRECTION! new BPM:");
                        Serial.print((int)bpm);
                    }
                }
            } else {
                halfTimeAgreement = 0;
            }
            Serial.println();
        }
    } else {
        // 🎯 仮想ビートで予測タイミングにLEDを光らせる
        // テンポロック中でもロック解除後でも、一度テンポが決まったら光り続ける
        if (estimatedInterval > 0 && lastVirtualBeatTime > 0) {
            uint32_t timeSinceVirtual = now - lastVirtualBeatTime;
            // 予測位置に達したか？
            if (timeSinceVirtual >= (uint32_t)estimatedInterval) {
                beatDetected = 1.0f;  // 仮想ビート発火！
                lastVirtualBeatTime = now;
                virtualBeatPhase++;
            } else {
                beatDetected *= 0.5f;  // 一瞬で減衰
            }
        } else {
            beatDetected *= 0.5f;  // まだテンポ未確定時は減衰
        }
    }
    
    // Debug output every 500ms to monitor noise levels
    static uint32_t lastDebugTime = 0;
    if (millis() - lastDebugTime > 500) {
        lastDebugTime = millis();
        Serial.print("[DBG] avgE:");
        Serial.print(avgEnergy, 4);
        Serial.print(" kickOn:");
        Serial.print(kickOnset, 4);
        Serial.print(" snareOn:");
        Serial.print(snareOnset, 4);
        Serial.print(" kTh:");
        Serial.print(kickThresh, 3);
        Serial.print(" sTh:");
        Serial.print(snareThresh, 3);
        Serial.print(" lock:");
        Serial.print(tempoLocked ? "YES" : "NO");
        Serial.print(" intv:");
        Serial.print((int)estimatedInterval);
        Serial.print(" conf:");
        Serial.print(tempoConfidence, 1);
        Serial.print(" pat:");
        Serial.print(patternNames[detectedPattern]);
        Serial.print("(");
        Serial.print(patternScore, 1);
        Serial.print(") silent:");
        Serial.println(isSilent ? "YES" : "NO");
    }
    
    // ============================================================
    // DISPLAY VALUES (separate from beat detection)
    // ============================================================
    float bassSum = 0, midSum = 0, highSum = 0;
    int bassCount = 0, midCount = 0, highCount = 0;
    
    for (int i = 1; i < SAMPLES / 2; i++) {
        float freq = i * binWidth;
        float mag = vReal[i];
        
        if (freq >= 60 && freq < 250) {
            bassSum += mag;
            bassCount++;
        } else if (freq >= 250 && freq < 2000) {
            midSum += mag;
            midCount++;
        } else if (freq >= 2000 && freq < 4000) {
            highSum += mag;
            highCount++;
        }
    }
    
    float newBass = bassCount > 0 ? bassSum / bassCount / 5000.0f : 0;
    float newMid = midCount > 0 ? midSum / midCount / 4000.0f : 0;
    float newHigh = highCount > 0 ? highSum / highCount / 3000.0f : 0;
    
    newBass = constrain(newBass, 0, 1);
    newMid = constrain(newMid, 0, 1);
    newHigh = constrain(newHigh, 0, 1);
    
    // Smoothing for display
    bassLevel = bassLevel * 0.5f + newBass * 0.5f;
    midLevel = midLevel * 0.5f + newMid * 0.5f;
    highLevel = highLevel * 0.5f + newHigh * 0.5f;
    overallVolume = (bassLevel + midLevel + highLevel) / 3.0f;
}

// Mic effect (spectrum style)
static float micHueOffset = 0;
void effectMic() {
    updateIMUColor();
    
    micHueOffset += 0.5f + overallVolume * 2.0f;  // Volume affects rotation speed
    
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
        
        // 💥 ビート検出時は全体が白くフラッシュ！
        float brightness = localLevel * 0.8f + 0.1f;
        float saturation = imuSaturation;
        
        if (beatDetected > 0.2f) {
            // ビート時、全体がフラッシュ
            brightness = min(1.0f, brightness + beatDetected * 0.8f);
            saturation = max(0.1f, saturation - beatDetected * 0.6f);
        }
        
        brightness = brightness * imuBrightness;
        pixels.setPixelColor(i, hsvToColor(hue, saturation, constrain(brightness, 0, 1)));
    }
}

// ============================================================
// 📝 POV (Persistence of Vision) - 残像テキスト表示
// "Unit Neco" をLEDの残像で空中に表示 ✨
//
// [ハードウェア] Unit Neco (U163) 🐱
//   PCBサイズ: 44.6 × 43.0mm（扇形/猫耳形状）
//   LED: WS2812C-2020 × 35個 → 外周の弧に沿って配置
//   2ユニットが直列接続 → LED 0-34(耳1), LED 35-69(耳2)
//
// [LED配置 - 外周弧上] 🔌
//
//        ╭── 17 ──╮      ← 頂点
//       /           \
//      8             26
//     /               \
//    0                 34  ← 底辺（コネクタ側）
//
// [POV方式] スイング時に片脚(LED 0-17)だけ使用 ⚡
//
//   左脚のみ使う理由:
//   スイング方向に対して左脚と右脚は ~44mm離れている。
//   両脚を同じタイミングで点灯すると二重像(ゴースト)になり
//   文字が読めない！片脚にすれば単一の縦線として描画される。
//
//        ╭── 17 ←使用  
//       /           (右脚は消灯)
//      8
//     /
//    0 ← 使用
//
//   → 18ピクセルの縦解像度で文字を描画
//   → 5×7フォント × 縦2倍 = 14/18ピクセル使用
//
// [テキスト分割] 🎯
//   耳1: "Unit" (LED 0-17 のみ使用)
//   耳2: "Neco" (LED 35-52 のみ使用)
//   → 2耳の物理的な隙間 = 自然な空白！
// ============================================================

// フォント定数
#define POV_FONT_W       5      // フォント幅（カラム）
#define POV_FONT_H       7      // フォント高さ（行）
#define POV_CHARS_EAR    4      // 各耳の文字数
#define POV_CHAR_GAP     1      // 文字間スペース（カラム）
#define POV_EAR_COLS     (POV_CHARS_EAR * POV_FONT_W + (POV_CHARS_EAR - 1) * POV_CHAR_GAP)
// = 4*5 + 3*1 = 23 カラム

// 縦方向マッピング (片脚: LED 0-17 = 18ピクセル)
#define POV_PIX_H        18     // 縦ピクセル数 (LED 0～17)
#define POV_VSCALE        2     // 縦2倍拡大 (7行×2=14段)
#define POV_VOFFSET       2     // 下マージン ((18-14)/2 = 2)

// 5×7 ピクセルフォント（カラム形式: bit0=文字上辺）
// 🐱 耳1用: "Unit"
const uint8_t povFontEar1[][POV_FONT_W] PROGMEM = {
    {0x3F, 0x40, 0x40, 0x40, 0x3F},  // 'U'
    {0x7C, 0x08, 0x04, 0x04, 0x78},  // 'n'
    {0x00, 0x44, 0x7D, 0x40, 0x00},  // 'i'
    {0x04, 0x3F, 0x44, 0x44, 0x24},  // 't'
};
// 🐱 耳2用: "Neco"
const uint8_t povFontEar2[][POV_FONT_W] PROGMEM = {
    {0x7F, 0x04, 0x08, 0x10, 0x7F},  // 'N'
    {0x38, 0x54, 0x54, 0x54, 0x18},  // 'e'
    {0x38, 0x44, 0x44, 0x44, 0x28},  // 'c'
    {0x38, 0x44, 0x44, 0x44, 0x38},  // 'o'
};

// 展開済みビットマップ 🖼️
uint8_t povBmpEar1[POV_EAR_COLS];
uint8_t povBmpEar2[POV_EAR_COLS];

// POV 状態変数
volatile int povCol = 0;
volatile bool povForward = true;
volatile uint32_t povLastColTime = 0;
float povGyroSmoothed = 0;
float povGyroRaw = 0;
bool povLastDir = true;
uint32_t povLastSwingTime = 0;
bool povActive = false;

// ビットマップ展開
void initPOVBitmap() {
    int c = 0;
    for (int ch = 0; ch < POV_CHARS_EAR; ch++) {
        for (int j = 0; j < POV_FONT_W; j++)
            povBmpEar1[c++] = pgm_read_byte(&povFontEar1[ch][j]);
        if (ch < POV_CHARS_EAR - 1) povBmpEar1[c++] = 0x00;
    }
    c = 0;
    for (int ch = 0; ch < POV_CHARS_EAR; ch++) {
        for (int j = 0; j < POV_FONT_W; j++)
            povBmpEar2[c++] = pgm_read_byte(&povFontEar2[ch][j]);
        if (ch < POV_CHARS_EAR - 1) povBmpEar2[c++] = 0x00;
    }
    Serial.print("📝 POV bitmap: ");
    Serial.print(POV_EAR_COLS);
    Serial.println(" cols/ear");
}

// ============================================================
// 1カラム表示 💡 ※片脚(LED 0-17)のみ使用！
//   → 右脚(LED 18-34)は消灯 = ゴースト防止の核心 ⭐
//   → WS2812プロトコル: 70LED全転送 ~2.1ms
// ============================================================
void IRAM_ATTR povShowColumn(int colIdx) {
    pixels.clear();  // 全70LED消灯

    if (colIdx < 0 || colIdx >= POV_EAR_COLS) {
        pixels.show();
        return;
    }

    uint8_t d1 = povBmpEar1[colIdx];
    uint8_t d2 = povBmpEar2[colIdx];
    uint32_t white = pixels.Color(255, 255, 255);  // 🔆 白=最大視認性

    for (int row = 0; row < POV_FONT_H; row++) {
        for (int s = 0; s < POV_VSCALE; s++) {
            // row 0 (文字上辺) → h大 (頂点LED17側) ☝️
            // row 6 (文字下辺) → h小 (底辺LED0側)  👇
            int h = POV_VOFFSET + (POV_FONT_H - 1 - row) * POV_VSCALE + s;
            if (h < 0 || h >= POV_PIX_H) continue;

            // 🐱 耳1 "Unit": LED h のみ（片脚!）
            if ((d1 >> row) & 1) {
                pixels.setPixelColor(h, white);
            }

            // 🐱 耳2 "Neco": LED (35 + h) のみ（片脚!）
            if ((d2 >> row) & 1) {
                pixels.setPixelColor(35 + h, white);
            }
        }
    }

    pixels.show();
}

// 📝 POV メインエフェクト ⚡
void effectPOV() {
    // === IMUなし: デモ自動スクロール ⏩ ===
    if (!imuReady) {
        static uint32_t t = 0;
        uint32_t now = micros();
        if (now - t > 1200) {  // 1.2ms間隔（高速デモ）
            t = now;
            povShowColumn(povCol);
            povCol = (povCol + 1) % POV_EAR_COLS;
        }
        return;
    }

    // === IMUデータ取得 📐 ===
    M5.Imu.update();
    auto imuData = M5.Imu.getImuData();
    float gx = imuData.gyro.x;
    float gy = imuData.gyro.y;
    float gz = imuData.gyro.z;

    // 3軸から最大の回転軸を選択 🔄
    float ax = fabsf(gx), ay = fabsf(gy), az = fabsf(gz);
    float dom = (az >= ax && az >= ay) ? gz : (ay >= ax ? gy : gx);

    povGyroRaw = dom;
    float absG = fabsf(dom);

    // 超応答スムージング 💨 ほぼ生値追従
    povGyroSmoothed = povGyroSmoothed * 0.05f + absG * 0.95f;

    // === 方向反転検出 🔃 ===
    bool curDir = dom >= 0;
    if (curDir != povLastDir && absG > 8.0f) {
        povForward = curDir;
        povCol = povForward ? 0 : (POV_EAR_COLS - 1);
        povLastDir = curDir;
        povLastColTime = micros();
        povLastSwingTime = millis();
    }

    // === POV表示制御 ⚡ ===
    const float SWING_THRESH = 15.0f;  // deg/s（低閾値で即座に反応）

    if (povGyroSmoothed > SWING_THRESH) {
        povActive = true;
        povLastSwingTime = millis();

        // テキスト表示弧 📐
        // 15°にコンパクト化 → 文字が圧縮されてシャープに見える！
        float arcDeg = 15.0f;
        float degPerCol = arcDeg / (float)POV_EAR_COLS;
        float colsPerSec = povGyroSmoothed / degPerCol;
        float interval_us = 1000000.0f / colsPerSec;

        // WS2812転送: ~1590us (53LED) が物理的下限 🏎️
        const float MIN_INT = 1700.0f;

        uint32_t now = micros();
        uint32_t elapsed = now - povLastColTime;

        if (interval_us >= MIN_INT) {
            if (elapsed >= (uint32_t)interval_us) {
                povLastColTime = now;
                povShowColumn(povCol);
                if (povForward) { if (povCol < POV_EAR_COLS - 1) povCol++; }
                else            { if (povCol > 0) povCol--; }
            }
        } else {
            // 高速スイング: カラムスキップ 🏃💨
            if (elapsed >= (uint32_t)MIN_INT) {
                int skip = constrain((int)(elapsed / interval_us), 1, 8);
                povLastColTime = now;
                if (povForward) povCol = min(povCol + skip, POV_EAR_COLS - 1);
                else            povCol = max(povCol - skip, 0);
                povShowColumn(povCol);
            }
        }
    } else {
        // === 待機中: 頂点が呼吸する 🌟 ===
        if (povActive && millis() - povLastSwingTime > 300) povActive = false;
        if (!povActive) {
            static uint32_t lt = 0;
            if (millis() - lt > 40) {
                lt = millis();
                pixels.clear();
                float b = (sinf(millis() * 0.005f) + 1.0f) * 0.5f;
                uint8_t br = (uint8_t)(b * 100);
                uint32_t c = pixels.Color(0, br, (uint8_t)(br * 0.7f));
                // 頂点3個 (LED 15,16,17 = 片脚のみ)
                for (int h = 15; h <= 17; h++) {
                    pixels.setPixelColor(h, c);
                    pixels.setPixelColor(35 + h, c);
                }
                pixels.show();
            }
        }
    }

    // === LCD表示 (500ms間隔) 📺 ===
    static uint32_t lastLcd = 0;
    if (millis() - lastLcd > 500) {
        lastLcd = millis();
        M5.Lcd.fillRect(0, 55, 320, 65, BLACK);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(CYAN);
        M5.Lcd.setCursor(10, 58);
        M5.Lcd.print("Unit | Neco");

        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setCursor(10, 78);
        M5.Lcd.print("Gyro:");
        M5.Lcd.print((int)povGyroSmoothed);
        M5.Lcd.print("d/s Raw:");
        M5.Lcd.print((int)povGyroRaw);

        M5.Lcd.setCursor(10, 90);
        M5.Lcd.print("Col:");
        M5.Lcd.print(povCol);
        M5.Lcd.print("/");
        M5.Lcd.print(POV_EAR_COLS);
        M5.Lcd.print(povForward ? " >>>" : " <<<");

        M5.Lcd.setCursor(10, 102);
        M5.Lcd.setTextColor(povActive ? GREEN : YELLOW);
        M5.Lcd.setTextSize(2);
        M5.Lcd.print(povActive ? "ACTIVE!" : "Swing me!");
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
    
    // 🔊 スピーカー初期化（ビート音用）
    Serial.println("🔊 Initializing Speaker...");
    auto spk_cfg = M5.Speaker.config();
    spk_cfg.sample_rate = 44100;
    spk_cfg.task_priority = 1;
    M5.Speaker.config(spk_cfg);
    M5.Speaker.begin();
    M5.Speaker.setVolume(speakerVolume);
    Serial.println("✅ Speaker ready!");
    
    // 猫耳LED初期化 🐱
    pixels.setBrightness(20);  // 🔥 LED焼損防止のため20に変更
    pixels.begin();
    pixels.clear();
    pixels.show();
    initPOVBitmap();  // 📝 POVビットマップ初期化
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
    // Touch check
    checkTouch();
    
    // 📝 POVモード: タイトなループで精密タイミング制御
    if (currentMode == MODE_POV) {
        effectPOV();
        return;
    }
    
    // Auto mode change (無効化)
    // checkAutoModeChange();
    
    // FFT/Beat detection - ALWAYS update every loop (independent of animation speed)
    updateMicFFT();
    
    // Debug display update (every 100ms)
    if (millis() - lastDebugUpdate >= DEBUG_UPDATE_INTERVAL) {
        lastDebugUpdate = millis();
        drawMicDebug();
        
        // Serial detailed output
        Serial.print("Bass:");
        Serial.print((int)(bassLevel * 100));
        Serial.print("% Mid:");
        Serial.print((int)(midLevel * 100));
        Serial.print("% High:");
        Serial.print((int)(highLevel * 100));
        Serial.print("% Beat:");
        Serial.print(beatDetected, 2);
        Serial.println();
    }
    
    // Animation update (speed varies by mode)
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
            case MODE_POV:     effectPOV();     break;
            default: break;
        }
        
        pixels.show();
    }
}

