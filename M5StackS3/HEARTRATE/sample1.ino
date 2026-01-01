/*
 * 使用ユニット / Units Used:
 *   - MAX30100 心拍センサー / Heart Rate Sensor (I2C)
 * 
 * ピン配置 / Pin Assignment:
 *   - MAX30100: PortA I2C (SDA=2, SCL=1)
 */

#include <M5CoreS3.h>
#include <Wire.h>
#include <math.h>
#include "MAX30100_PulseOximeter.h"

#define SAMPLING_RATE   (MAX30100_SAMPRATE_100HZ)
#define IR_LED_CURRENT  (MAX30100_LED_CURR_24MA)
#define RED_LED_CURRENT (MAX30100_LED_CURR_27_1MA)
#define PULSE_WIDTH     (MAX30100_SPC_PW_1600US_16BITS)
#define HIGHRES_MODE    (true)
#define REPORTING_PERIOD_MS     1000

// PulseOximeter is the higher level interface to the sensor
// it offers:
//  * beat detection reporting
//  * heart rate calculation５
//  * SpO2 (oxidation level) calculation
PulseOximeter pox;
uint32_t tsLastReport = 0;

uint16_t getColor(uint8_t red, uint8_t green, uint8_t blue){
  return ((red>>3)<<11) | ((green>>2)<<5) | (blue>>3);
}
MAX30100 sensor;  // Instantiate a MAX30100 sensor class.  实例化一个MAX30100传感器类
bool beatflg;
void onBeatDetected()
{
    Serial.println("Beat!");
    if (beatflg) {
    M5.Lcd.fillCircle(30, 40, 10, BLACK);
    M5.Lcd.fillCircle(50, 40, 10, BLACK);
    M5.Lcd.fillCircle(40, 41, 3, BLACK);
    M5.Lcd.fillTriangle(22, 45, 58, 45, 40, 65, BLACK);
    beatflg = false;
    }
    else {
    M5.Lcd.fillCircle(30, 40, 10, RED);
    M5.Lcd.fillCircle(50, 40, 10, RED);
    M5.Lcd.fillCircle(40, 41, 3, RED);
    M5.Lcd.fillTriangle(22, 45, 58, 45, 40, 65, RED);
    beatflg = true;
    }
}


void setup()
{
    M5.begin();        // Init M5Stack.  初始化M5Stack
    M5.Power.begin();  // Init power  初始化电源模块
    Serial.print("Initializing MAX30100..");
 // ハートを表示
    M5.Lcd.fillCircle(30, 40, 10, RED);
    M5.Lcd.fillCircle(50, 40, 10, RED);
    M5.Lcd.fillCircle(40, 41, 3, RED);
    M5.Lcd.fillTriangle(22, 45, 58, 45, 40, 65, RED);

    // O2を表示
    M5.Lcd.setTextSize(3);
    M5.Lcd.setCursor(20, 80);
    M5.Lcd.print("O");
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(48, 100);
    M5.Lcd.print("2");

    while (!sensor.begin()) {  // Initialize the sensor.  初始化传感器
    //    M5.Lcd.setTextFont(4);
    //    M5.Lcd.setCursor(50, 100, 4);
    //    M5.Lcd.println("Sensor not found");
        delay(1000);
    }
    //M5.Lcd.fillScreen(BLACK);
    // Set up the wanted parameters.  设置所需的参数
    sensor.setMode(MAX30100_MODE_SPO2_HR);
    sensor.setLedsCurrent(IR_LED_CURRENT, RED_LED_CURRENT);
    sensor.setLedsPulseWidth(PULSE_WIDTH);
    sensor.setSamplingRate(SAMPLING_RATE);
    sensor.setHighresModeEnabled(HIGHRES_MODE);
    pox.setOnBeatDetectedCallback(onBeatDetected);
}

void loop()
{
    // Make sure to call update as fast as possible
    pox.update();

    // Asynchronously dump heart rate and oxidation levels to the serial
    // For both, a value of 0 means "invalid"
    if (millis() - tsLastReport > REPORTING_PERIOD_MS) {
        M5.Lcd.setTextSize(3);

        // 心拍数を表示
        M5.Lcd.setCursor(75, 40);
        M5.Lcd.print(pox.getHeartRate());

        //　血中酸素量を表示
        M5.Lcd.setCursor(75, 90);
        M5.Lcd.print(pox.getSpO2());

        tsLastReport = millis();
    }
}
