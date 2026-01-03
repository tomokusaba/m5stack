# M5Stack CoreS3 サンプル集

このリポジトリは M5Stack CoreS3（ESP32-S3）向けの Arduino サンプルスケッチをまとめたものです。心拍センサー、IMU、ネコ型 NeoPixel ユニットなど複数のユニットを CoreS3 に接続して動かす例を含みます。

## ディレクトリ構成

- `M5StackS3/HEARTRATE/` – MAX30100 心拍センサーを利用したサンプル
- `M5StackS3/IMU/` – 内蔵 IMU（BMI270/BMM150）と microSD を使ったサンプル
- `M5StackS3/NECO/` – NECO Unit（NeoPixel LED）制御サンプル

各フォルダーに複数の `.ino` ファイルがあり、コメント内に接続ポートやピン配置が記載されています。

## 必要環境

- Arduino IDE 2.x または PlatformIO（ESP32 ボードパッケージが導入された環境）
- ボード設定: **M5Stack CoreS3**（ESP32-S3）
- 利用ライブラリ例  
  - `M5CoreS3`（M5Stack 公式ライブラリ）  
  - `MAX30100_PulseOximeter`  
  - `Adafruit NeoPixel`  
  - 標準の `SD` / `SPI` / `Wire` ライブラリ

## 使い方

1. 必要ライブラリと CoreS3 ボード定義をインストールします。  
2. 該当するサンプル `.ino` を Arduino IDE で開き、コメントに記載のポート／ピンにユニットを接続します。  
3. ボードに **M5Stack-CoreS3**, ポートに接続中のデバイスを選択し、ビルド＆書き込みを行います。  
4. 動作確認はシリアルモニターや液晶表示、NeoPixel の発光状態などで行ってください。

## 補足

- センサーや LED の電源電圧・向きに注意してください。  
- microSD を利用するサンプルでは FAT32 でフォーマットしたカードを使用してください。  
- 詳細な配線図やピン割り当ては各サンプル冒頭のコメントを参照してください。
