/*
 * MOSS 语音助手 —— 喇叭 / 功放自检程序（MAX98357A）
 * =================================================
 * 用途：脱离上位机与 Wi-Fi，单独验证「ESP32-C3 + MAX98357A + 喇叭」通路是否正常，
 *       并打印每一步的返回值，便于定位是接线还是供电问题。
 *
 * 不依赖任何第三方库（只用 Arduino 核心自带库），可直接编译上传。
 *
 * 接线（与 moss_firmware.ino 一致，详见 docs/接线方案与端口定义.md）：
 *   MAX98357A VIN  -> 5V
 *   MAX98357A GND  -> GND（必须与 ESP32 共地）
 *   MAX98357A BCLK -> GPIO4
 *   MAX98357A LRC  -> GPIO5
 *   MAX98357A DIN  -> GPIO7
 *   MAX98357A SD   -> VIN（拉高使能，悬空会静音）
 *   MAX98357A GAIN -> 悬空或 GND
 *   喇叭 +/-       -> MAX98357A OUT+ / OUT-（BTL 输出，不要接 GND）
 *
 * 串口命令（波特率 115200）：
 *   h  显示帮助
 *   t  播放 1kHz 测试音 1 秒（最基本的通断测试）
 *   s  扫频 200Hz -> 4000Hz（听是否有断点/失真）
 *   m  播放《小星星》旋律（确认音调正确）
 *   r  采样率测试：同样的 1kHz 分别用 16k / 22.05k / 44.1k 播放（三遍音高应一致）
 *   v  音量阶梯测试：幅度 10% -> 80%（确认增益与功放是否够响）
 *   i  重新初始化 I2S 并打印全部返回值
 */

#include <Arduino.h>
#include "driver/i2s.h"
#include "esp_err.h"

// ---------------- 引脚（与主固件一致） ----------------
#define PIN_BCLK       4
#define PIN_LRCLK      5
#define PIN_SPK_DOUT   7

#define I2S_PORT       I2S_NUM_0
#define DEFAULT_RATE   16000
#define TONE_BUF       512          // 每次写入的样本数

static int16_t toneBuf[TONE_BUF];

// ================= 工具：打印返回值 =================
static void showErr(const char* what, esp_err_t err) {
  Serial.printf("  %-30s -> %-4d  %s%s\n",
                what, (int)err, esp_err_to_name(err),
                (err == ESP_OK) ? "   [OK]" : "   <== 出错！");
}

// ================= I2S 播放初始化 =================
static bool i2sInitTx(uint32_t sampleRate) {
  Serial.printf("\n---- 初始化 I2S（播放 TX，%lu Hz）----\n", (unsigned long)sampleRate);

  esp_err_t e = i2s_driver_uninstall(I2S_PORT);
  showErr("i2s_driver_uninstall()", e);      // 首次返回 ESP_ERR_INVALID_STATE 属正常

  i2s_config_t config = {};
  config.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate          = sampleRate;
  config.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_I2S;
  config.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count        = 8;
  config.dma_buf_len          = 256;
  config.use_apll             = false;
  config.tx_desc_auto_clear   = true;
  config.fixed_mclk           = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num   = PIN_BCLK;
  pins.ws_io_num    = PIN_LRCLK;
  pins.data_out_num = PIN_SPK_DOUT;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;

  e = i2s_driver_install(I2S_PORT, &config, 0, nullptr);
  showErr("i2s_driver_install()", e);
  if (e != ESP_OK) return false;

  e = i2s_set_pin(I2S_PORT, &pins);
  showErr("i2s_set_pin()", e);

  e = i2s_zero_dma_buffer(I2S_PORT);
  showErr("i2s_zero_dma_buffer()", e);

  Serial.printf("  引脚: BCLK=GPIO%d  LRC=GPIO%d  DIN=GPIO%d\n",
                PIN_BCLK, PIN_LRCLK, PIN_SPK_DOUT);
  return true;
}

// ================= 播放一段频率/幅度可调的正弦音 =================
static void playTone(uint32_t sampleRate, double freq, uint32_t ms, double amp, const char* tag) {
  static double phase = 0.0;
  uint32_t total = (uint32_t)((double)sampleRate * ms / 1000.0);
  uint32_t done = 0;
  uint32_t written = 0;
  int failures = 0;
  esp_err_t lastErr = ESP_OK;

  while (done < total && failures < 4) {
    size_t n = TONE_BUF;
    if (n > total - done) n = total - done;

    for (size_t i = 0; i < n; i++) {
      toneBuf[i] = (int16_t)(sin(phase) * 32767.0 * amp);
      phase += 2.0 * PI * freq / (double)sampleRate;
      if (phase > 2.0 * PI) phase -= 2.0 * PI;
    }

    size_t w = 0;
    esp_err_t e = i2s_write(I2S_PORT, toneBuf, n * sizeof(int16_t), &w, pdMS_TO_TICKS(1000));
    if (e != ESP_OK || w != n * sizeof(int16_t)) {
      failures++;
      lastErr = e;
      Serial.printf("  i2s_write 返回 %d (%s)  期望 %u 实际 %u 字节  <== 写入异常\n",
                    (int)e, esp_err_to_name(e),
                    (unsigned)(n * sizeof(int16_t)), (unsigned)w);
    }
    written += w;
    done += n;
  }

  Serial.printf("[%s] %luHz %lums 幅度%.0f%%: 写入 %lu 字节, 失败 %d 次, 最后错误 %d (%s)\n",
                tag, (unsigned long)freq, (unsigned long)ms, amp * 100,
                (unsigned long)written, failures, (int)lastErr, esp_err_to_name(lastErr));
}

// ================= 各项测试 =================
static void testSingle() {
  Serial.println("\n===== 测试 1：1kHz 单音（最基本的通断测试）=====");
  if (!i2sInitTx(DEFAULT_RATE)) return;
  playTone(DEFAULT_RATE, 1000, 1000, 0.25, "1kHz");
  Serial.println("  >>> 听到「嘟——」= 功放+喇叭正常；没声音先查 SD 是否接 VIN(5V) 与共地");
}

static void testSweep() {
  Serial.println("\n===== 测试 2：扫频 200Hz -> 4000Hz =====");
  if (!i2sInitTx(DEFAULT_RATE)) return;
  for (double fr = 200; fr <= 4000; fr += 200) {
    playTone(DEFAULT_RATE, fr, 120, 0.25, "扫频");
  }
  Serial.println("  >>> 应听到连续上扬的音调，无断续/爆音");
}

static void testMelody() {
  Serial.println("\n===== 测试 3：《小星星》旋律（确认音调正确）=====");
  if (!i2sInitTx(DEFAULT_RATE)) return;
  // C4 D4 E4 F4 G4 A4
  const double notes[] = {262, 262, 392, 392, 440, 440, 392,
                          349, 349, 330, 330, 294, 294, 262};
  const uint32_t beat = 350;
  for (int i = 0; i < (int)(sizeof(notes) / sizeof(notes[0])); i++) {
    playTone(DEFAULT_RATE, notes[i], beat, 0.25, "旋律");
    delay(40);
  }
  Serial.println("  >>> 旋律应能听出「一闪一闪亮晶晶」");
}

static void testSampleRates() {
  Serial.println("\n===== 测试 4：采样率一致性（同样的 1kHz 用不同采样率播放）=====");
  const uint32_t rates[] = {16000, 22050, 44100};
  for (int i = 0; i < 3; i++) {
    if (!i2sInitTx(rates[i])) return;
    playTone(rates[i], 1000, 600, 0.25, "采样率");
    delay(200);
  }
  Serial.println("  >>> 三遍音高应完全相同；若某遍明显变调，说明采样率配置有问题");
}

static void testVolumeSteps() {
  Serial.println("\n===== 测试 5：音量阶梯（10% -> 80%）=====");
  if (!i2sInitTx(DEFAULT_RATE)) return;
  for (double a = 0.1; a <= 0.81; a += 0.1) {
    playTone(DEFAULT_RATE, 1000, 400, a, "音量");
    delay(150);
  }
  Serial.println("  >>> 音量应逐级变大；若最大音量仍很小，检查功放 GAIN 引脚与电源(5V/2A)");
}

static void showHelp() {
  Serial.println("\n================ 命令 ================");
  Serial.println("  h  显示本帮助");
  Serial.println("  t  1kHz 单音 1 秒（通断测试）");
  Serial.println("  s  扫频 200Hz -> 4000Hz");
  Serial.println("  m  播放《小星星》旋律");
  Serial.println("  r  采样率测试（16k / 22.05k / 44.1k）");
  Serial.println("  v  音量阶梯 10% -> 80%");
  Serial.println("  i  重新初始化 I2S 并打印全部返回值");
  Serial.println("=====================================\n");
}

void setup() {
  Serial.begin(115200);
  delay(1200);                       // 等 USB CDC 串口稳定

  Serial.println("\n\n############################################");
  Serial.println("#  MOSS 语音助手 —— 喇叭/功放自检 v1.0     #");
  Serial.println("#  ESP32-C3 + MAX98357A + 喇叭              #");
  Serial.println("############################################");
  Serial.printf("芯片=%s rev%d  Flash=%lu 字节  空闲内存=%lu 字节\n",
                ESP.getChipModel(), ESP.getChipRevision(),
                (unsigned long)ESP.getFlashChipSize(), (unsigned long)ESP.getFreeHeap());

  showHelp();
  Serial.println("正在自动播放一次 1kHz 自检音……（输入 t 可重播）");
  testSingle();
}

void loop() {
  if (!Serial.available()) {
    delay(50);
    return;
  }

  char c = Serial.read();
  switch (c) {
    case 'h': case 'H': showHelp(); break;
    case 't': case 'T': testSingle(); break;
    case 's': case 'S': testSweep(); break;
    case 'm': case 'M': testMelody(); break;
    case 'r': case 'R': testSampleRates(); break;
    case 'v': case 'V': testVolumeSteps(); break;
    case 'i': case 'I': i2sInitTx(DEFAULT_RATE); break;
    case '\r': case '\n': break;
    default: Serial.printf("\n未知命令 '%c'，输入 h 查看帮助\n", c); break;
  }
}
