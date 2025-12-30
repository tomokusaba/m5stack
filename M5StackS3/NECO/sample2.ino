// 🐱 猫耳LED マルチモード スケッチ
// タッチボタンで楽しいエフェクトを切り替え！ ✨

#include <M5CoreS3.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>

#define PIN        2        // PortB 🐱
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
    {  5, 170, 100, 60, 0x07E0, "CHASE",   MODE_CHASE},    // 緑
    {110, 170, 100, 60, 0xFFE0, "BLINK",   MODE_BLINK},    // 黄
    {215, 170, 100, 60, 0xF81F, "RAINBOW", MODE_RAINBOW},  // マゼンタ
    {  5, 105, 100, 60, 0x07FF, "SPARKLE", MODE_SPARKLE},  // シアン
    {110, 105, 100, 60, 0xFD20, "BREATHE", MODE_BREATHE},  // オレンジ
    {215, 105, 100, 60, 0xF800, "PARTY",   MODE_PARTY},    // 赤
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

// 速度設定
int speeds[] = {30, 300, 20, 50, 30, 40};  // 各モードの更新間隔

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
    M5.Lcd.fillRect(0, 0, 320, 100, BLACK);
    M5.Lcd.setTextSize(3);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(30, 20);
    M5.Lcd.print("NECO PARTY!");
    
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(CYAN);
    M5.Lcd.setCursor(50, 60);
    M5.Lcd.print("Mode: ");
    M5.Lcd.setTextColor(YELLOW);
    M5.Lcd.print(buttons[currentMode].label);
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
                    currentMode = btn.mode;
                    animPosition = 0;
                    hueOffset = 0;
                    breathValue = 0;
                    breathDir = 0.05;
                    
                    Serial.print("🎮 Mode changed: ");
                    Serial.println(btn.label);
                    
                    drawTitle();
                    drawButtons();
                    break;
                }
            }
        }
    }
}

// === エフェクト関数 ===

// 🌊 チェイス（流れる光）
void effectChase() {
    pixels.clear();
    int trailLen = 12;
    
    for (int i = 0; i < trailLen; i++) {
        int pos = (animPosition - i + NUMPIXELS) % NUMPIXELS;
        float brightness = 1.0 - ((float)i / trailLen);
        brightness = brightness * brightness;
        
        uint32_t color = hsvToColor((pos * 4 + hueOffset) % 256, 1.0, brightness);
        pixels.setPixelColor(pos, color);
    }
    
    animPosition = (animPosition + 1) % NUMPIXELS;
    hueOffset = (hueOffset + 2) % 256;
}

// 💡 ブリンク（点滅）
void effectBlink() {
    if (blinkState) {
        int hue = hueOffset;
        for (int i = 0; i < NUMPIXELS; i++) {
            pixels.setPixelColor(i, hsvToColor((hue + i * 3) % 256, 1.0, 1.0));
        }
    } else {
        pixels.clear();
    }
    blinkState = !blinkState;
    hueOffset = (hueOffset + 20) % 256;
}

// 🌈 レインボー（虹色グラデーション）
void effectRainbow() {
    for (int i = 0; i < NUMPIXELS; i++) {
        int hue = (i * 256 / NUMPIXELS + hueOffset) % 256;
        pixels.setPixelColor(i, hsvToColor(hue, 1.0, 1.0));
    }
    hueOffset = (hueOffset + 3) % 256;
}

// ✨ スパークル（キラキラ）
void effectSparkle() {
    // 徐々に暗くする
    for (int i = 0; i < NUMPIXELS; i++) {
        uint32_t c = pixels.getPixelColor(i);
        int r = ((c >> 16) & 0xFF) * 0.85;
        int g = ((c >> 8) & 0xFF) * 0.85;
        int b = (c & 0xFF) * 0.85;
        pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
    
    // ランダムにキラッと光らせる
    for (int i = 0; i < 5; i++) {
        if (random(100) < 40) {
            int pos = random(NUMPIXELS);
            pixels.setPixelColor(pos, hsvToColor(random(256), 0.5, 1.0));
        }
    }
}

// 💨 ブリーズ（呼吸）
void effectBreathe() {
    breathValue += breathDir;
    if (breathValue >= 1.0) {
        breathValue = 1.0;
        breathDir = -0.03;
    } else if (breathValue <= 0.05) {
        breathValue = 0.05;
        breathDir = 0.03;
        hueOffset = (hueOffset + 30) % 256;
    }
    
    for (int i = 0; i < NUMPIXELS; i++) {
        pixels.setPixelColor(i, hsvToColor(hueOffset, 1.0, breathValue));
    }
}

// 🎉 パーティー
void effectParty() {
    for (int i = 0; i < NUMPIXELS; i++) {
        if (random(100) < 30) {
            pixels.setPixelColor(i, hsvToColor(random(256), 1.0, 1.0));
        } else {
            uint32_t c = pixels.getPixelColor(i);
            int r = ((c >> 16) & 0xFF) * 0.7;
            int g = ((c >> 8) & 0xFF) * 0.7;
            int b = (c & 0xFF) * 0.7;
            pixels.setPixelColor(i, pixels.Color(r, g, b));
        }
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
    
    // 猫耳LED初期化 🐱
    pixels.setBrightness(40);
    pixels.begin();
    pixels.clear();
    pixels.show();
    Serial.println("✅ NECO OK!");
    
    // 画面初期化 📺
    M5.Lcd.fillScreen(BLACK);
    drawTitle();
    drawButtons();
    
    Serial.println("✨ Touch buttons to change mode!");
}

void loop()
{
    // タッチチェック
    checkTouch();
    
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
            default: break;
        }
        
        pixels.show();
    }
}
