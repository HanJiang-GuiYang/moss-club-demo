/*
 * MOSS 语音助手 —— ESP32-C3 固件
 *
 * 功能：
 *   - Wi-Fi 连接
 *   - 轻触开关（GPIO1）触发 4 秒录音
 *   - I2S 麦克风 INMP441 采集 16kHz 单声道 PCM
 *   - HTTP POST 上传到上位机 /api/command
 *   - 接收 JSON（reply + tts_url）
 *   - 从 tts_url 下载 WAV：解析 RIFF/fmt/data 块，按 WAV 实际采样率/声道/位深播放
 *   - 喇叭自检：开机提示音（SPEAKER_BEEP_ON_BOOT）+ 串口发送 T 播放 1kHz 测试音
 *   - WS2812 灯环（GPIO10）显示状态
 *
 * 依赖库（Arduino IDE 库管理器安装）：
 *   - ArduinoJson
 *   - Adafruit NeoPixel
 *
 * 接线与 GPIO 映射（详见 docs/接线方案与端口定义.md）：
 *   I2S BCLK   -> GPIO4   （麦克风 SCK 与功放 BCLK 共用）
 *   I2S LRCLK  -> GPIO5   （麦克风 WS  与功放 LRC  共用）
 *   麦克风数据  -> GPIO6   （INMP441 SD   → ESP32，输入）
 *   功放数据   -> GPIO7   （ESP32 → MAX98357A DIN，输出）
 *   录音按钮   -> GPIO1   （内部上拉，按下为 LOW）
 *   WS2812 DIN -> GPIO10  （灯环数据）
 *
 * 串口命令（115200 波特率）：
 *   T  播放 1kHz 测试音（喇叭/功放硬件自检，不依赖 Wi-Fi 与上位机）
 *
 * 注意：ESP32-C3 只有一个 I2S0，因此录音(RX)与播放(TX)复用同一端口，
 *       在 installI2SRx() / installI2STx() 之间切换模式。
 *       所有 GPIO 请以实际开发板丝印为准修改。
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include "driver/i2s.h"

// ---------------- 用户配置 ----------------
const char* WIFI_SSID = "你的WiFi名称";
const char* WIFI_PASS = "你的WiFi密码";

// 改成电脑在局域网中的固定 IP
const char* SERVER_URL = "http://192.168.1.100:8765";

// ---------------- 自检开关 ----------------
#define SPEAKER_BEEP_ON_BOOT   1        // 开机“嘟”一声做喇叭/功放自检（不需要联网，改成 0 可关闭）
#define WAV_READ_TIMEOUT_MS    3000     // 等待 WAV 数据的最长时间
#define WAV_CHUNK_BYTES        1024     // 每次从 HTTP 流读取的 WAV 数据块大小
#define WAV_MAX_FMT_CHUNK      16       // 解析 fmt 块需要的字节数

// ---------------- GPIO 定义 ----------------
#define PIN_BCLK       4
#define PIN_LRCLK      5
#define PIN_MIC_DIN    6   // INMP441 SD -> ESP32
#define PIN_SPK_DOUT   7   // ESP32 -> MAX98357A DIN
#define PIN_BUTTON     1   // 轻触开关，内部上拉
#define PIN_LED_RING   10  // WS2812 DIN

// ---------------- I2S / 采样参数 ----------------
#define I2S_PORT         I2S_NUM_0
#define SAMPLE_RATE      16000
#define RECORD_SECONDS   4
#define SAMPLE_COUNT     (SAMPLE_RATE * RECORD_SECONDS)

// ---------------- WS2812 灯环 ----------------
#define LED_COUNT        16
Adafruit_NeoPixel ring(LED_COUNT, PIN_LED_RING, NEO_GRB + NEO_KHZ800);

// 状态颜色（R,G,B）
const uint32_t COLOR_IDLE      = ring.Color(0, 0, 80);    // 蓝：待机
const uint32_t COLOR_RECORDING = ring.Color(120, 0, 0);   // 红：录音中
const uint32_t COLOR_THINKING  = ring.Color(0, 120, 0);   // 绿：处理/联网中
const uint32_t COLOR_PLAYING  = ring.Color(80, 0, 120);   // 紫：播放中
const uint32_t COLOR_ERROR    = ring.Color(120, 60, 0);   // 橙：错误

static int16_t pcmBuffer[SAMPLE_COUNT];

// ---------------- 灯环状态显示 ----------------
void setRingColor(uint32_t color) {
  ring.fill(color, 0, LED_COUNT);
  ring.show();
}

void ringBlink(uint32_t color, int times, int delayMs = 150) {
  for (int i = 0; i < times; i++) {
    setRingColor(color);
    delay(delayMs);
    setRingColor(ring.Color(0, 0, 0));
    delay(delayMs);
  }
}

// ---------------- I2S 配置（公用参数） ----------------
void installI2SRx() {
  i2s_driver_uninstall(I2S_PORT);

  i2s_config_t config = {};
  config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;   // INMP441 L/R 接地 = 左声道
  config.communication_format = I2S_COMM_FORMAT_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = 256;
  config.use_apll = false;
  config.tx_desc_auto_clear = false;
  config.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = PIN_BCLK;
  pins.ws_io_num = PIN_LRCLK;
  pins.data_out_num = I2S_PIN_NO_CHANGE;   // 录音模式不用输出
  pins.data_in_num = PIN_MIC_DIN;

  i2s_driver_install(I2S_PORT, &config, 0, nullptr);
  i2s_set_pin(I2S_PORT, &pins);
  i2s_zero_dma_buffer(I2S_PORT);
}

// 播放模式：采样率按 WAV 实际参数传入（默认 16kHz）
void installI2STx(uint32_t sampleRate = SAMPLE_RATE) {
  i2s_driver_uninstall(I2S_PORT);

  i2s_config_t config = {};
  config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = sampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = 256;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;
  config.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = PIN_BCLK;
  pins.ws_io_num = PIN_LRCLK;
  pins.data_out_num = PIN_SPK_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  i2s_driver_install(I2S_PORT, &config, 0, nullptr);
  i2s_set_pin(I2S_PORT, &pins);
  i2s_zero_dma_buffer(I2S_PORT);
}

// ---------------- Wi-Fi ----------------
bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    ringBlink(COLOR_THINKING, 1, 250);
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    setRingColor(COLOR_IDLE);
  } else {
    ringBlink(COLOR_ERROR, 5, 200);
  }
  return WiFi.status() == WL_CONNECTED;
}

// ---------------- 录音 ----------------
void recordPcm() {
  installI2SRx();
  setRingColor(COLOR_RECORDING);

  int32_t raw[256];
  size_t bytesRead = 0;
  int index = 0;

  while (index < SAMPLE_COUNT) {
    i2s_read(I2S_PORT, raw, sizeof(raw), &bytesRead, portMAX_DELAY);

    int count = bytesRead / sizeof(int32_t);
    for (int i = 0; i < count && index < SAMPLE_COUNT; i++) {
      // INMP441 常见输出为左对齐 24 位，右移后转 16 位
      int32_t sample = raw[i] >> 14;
      if (sample > 32767) sample = 32767;
      if (sample < -32768) sample = -32768;
      pcmBuffer[index++] = (int16_t)sample;
    }
  }
  installI2STx(); // 释放 RX，切回可用状态
}

// ---------------- 喇叭 / 功放自检 ----------------
// 播放一段正弦提示音，用于脱离上位机验证 MAX98357A + 喇叭是否正常。
// 串口发送 T 可随时调用；setup() 里由 SPEAKER_BEEP_ON_BOOT 决定是否开机自动播一次。
void playTestTone(uint16_t freq = 1000, uint16_t durationMs = 300, float amplitude = 0.25f) {
  installI2STx(SAMPLE_RATE);
  setRingColor(COLOR_PLAYING);

  static int16_t buf[512];
  uint32_t total = (uint32_t)SAMPLE_RATE * durationMs / 1000;   // 总样本数
  uint32_t done = 0;
  uint32_t written = 0;
  uint16_t failures = 0;
  esp_err_t lastErr = ESP_OK;

  while (done < total && failures < 4) {
    size_t n = sizeof(buf) / sizeof(buf[0]);
    if (n > total - done) n = total - done;

    for (size_t i = 0; i < n; i++) {
      double t = (double)(done + i) / SAMPLE_RATE;
      buf[i] = (int16_t)(sin(2.0 * PI * freq * t) * 32767.0 * amplitude);
    }

    size_t w = 0;
    esp_err_t e = i2s_write(I2S_PORT, buf, n * sizeof(int16_t), &w, pdMS_TO_TICKS(1000));
    if (e != ESP_OK || w != n * sizeof(int16_t)) {
      failures++;
      lastErr = e;
      Serial.printf("[自检] i2s_write 返回 %d (%s)\n", (int)e, esp_err_to_name(e));
    }
    written += w;
    done += n;
  }

  Serial.printf("[自检] 喇叭测试音 %uHz/%ums: 写入 %lu 字节, 失败 %u 次, 最后错误 %d (%s)\n",
                freq, durationMs, (unsigned long)written, failures,
                (int)lastErr, esp_err_to_name(lastErr));
  if (written > 0 && failures == 0) {
    Serial.println("[自检] 应能听到提示音；若无声音，检查功放 SD 是否接 VIN(5V)、喇叭是否接 OUT+/OUT-、是否共地");
  }
  setRingColor(COLOR_IDLE);
}

// ---------------- WAV 头解析 ----------------
// 不再假设固定 44 字节头：按 RIFF 结构逐块扫描并定位 fmt / data，
// 这样可支持带扩展块（LIST、fact 等）的 WAV，并按 WAV 实际参数播放。
// 若引入非 PCM 编码或超过 2 声道，必须同时修改本文件与 docs/软件接口协议.md。
struct WavInfo {
  uint32_t sampleRate = SAMPLE_RATE;
  uint32_t dataSize   = 0;      // data 块负载字节数
  uint16_t channels   = 1;
  uint16_t bits       = 16;
  uint16_t formatTag  = 1;      // 1 = PCM
  bool     valid      = false;
};

static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 从 HTTP 流读取最多 n 字节（有数据则刷新超时），返回实际读到的字节数
static size_t readUpTo(WiFiClient* stream, uint8_t* dst, size_t n) {
  unsigned long start = millis();
  size_t got = 0;
  while (got < n && millis() - start < WAV_READ_TIMEOUT_MS) {
    int avail = stream->available();
    if (avail <= 0) {
      delay(2);
      continue;
    }
    size_t want = (n - got < (size_t)avail) ? (n - got) : (size_t)avail;
    int r = stream->read(dst + got, want);
    if (r > 0) {
      got += r;
      start = millis();
    }
  }
  return got;
}

static bool readFully(WiFiClient* stream, uint8_t* dst, size_t n) {
  return readUpTo(stream, dst, n) == n;
}

// HTTP 流不能 seek，不需要的字节只能读掉
static bool skipBytes(WiFiClient* stream, uint32_t n) {
  uint8_t scratch[64];
  while (n > 0) {
    size_t want = (n < sizeof(scratch)) ? (size_t)n : sizeof(scratch);
    size_t got = readUpTo(stream, scratch, want);
    if (got == 0) return false;
    n -= got;
  }
  return true;
}

// 解析 RIFF/WAVE 头，返回时数据流正好停在 data 块负载的起始位置
static bool parseWavHeader(WiFiClient* stream, WavInfo& info) {
  uint8_t riff[12];
  if (!readFully(stream, riff, sizeof(riff))) return false;
  if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) return false;

  bool haveFmt = false;
  for (int guard = 0; guard < 64; guard++) {   // 防止异常文件导致死循环
    uint8_t ch[8];
    if (!readFully(stream, ch, sizeof(ch))) return false;
    uint32_t size = le32(ch + 4);

    if (memcmp(ch, "fmt ", 4) == 0) {
      uint8_t fmt[WAV_MAX_FMT_CHUNK] = {0};
      size_t want = (size < sizeof(fmt)) ? (size_t)size : sizeof(fmt);
      if (want > 0 && !readFully(stream, fmt, want)) return false;
      info.formatTag  = le16(fmt + 0);
      info.channels   = le16(fmt + 2);
      info.sampleRate = le32(fmt + 4);
      info.bits       = le16(fmt + 14);
      haveFmt = true;
      if (size > want && !skipBytes(stream, size - (uint32_t)want)) return false;
    } else if (memcmp(ch, "data", 4) == 0) {
      info.dataSize = size;
      info.valid = haveFmt;
      return haveFmt;
    } else {
      if (size > 0 && !skipBytes(stream, size)) return false;
    }

    if (size & 1) {                            // 块按字对齐，奇数长度补 1 字节
      if (!skipBytes(stream, 1)) return false;
    }
  }
  return false;
}

// 把一段 WAV 数据转成 I2S 需要的“交错立体声 16bit”
// 单声道会复制成左右两路（否则以立体声模式播放会变调、变慢）；
// 返回写入 out 的字节数，格式不支持或容量不足时返回 0。
static size_t wavToStereo16(const uint8_t* in, size_t inLen, const WavInfo& info,
                            int16_t* out, size_t outCapSamples) {
  if (info.formatTag != 1) return 0;                       // 只支持 PCM
  if (info.bits != 16 && info.bits != 8) return 0;          // 只支持 8/16bit
  if (info.channels < 1 || info.channels > 2) return 0;     // 只支持单/双声道

  size_t bytesPerSample = info.bits / 8;
  size_t frameBytes = bytesPerSample * info.channels;
  size_t frames = inLen / frameBytes;
  if (frames * 2 > outCapSamples) return 0;

  for (size_t f = 0; f < frames; f++) {
    const uint8_t* p = in + f * frameBytes;
    int16_t l, r;
    if (info.bits == 16) {
      l = (int16_t)le16(p);
      r = (info.channels == 2) ? (int16_t)le16(p + 2) : l;
    } else {                                                // 8bit WAV 为无符号
      l = (int16_t)(((int16_t)p[0] - 128) << 8);
      r = (info.channels == 2) ? (int16_t)(((int16_t)p[1] - 128) << 8) : l;
    }
    out[f * 2]     = l;
    out[f * 2 + 1] = r;
  }
  return frames * 2 * sizeof(int16_t);
}

// ---------------- 播放 WAV ----------------
void playWavFromUrl(const String& url) {
  HTTPClient http;
  if (!http.begin(url)) {
    Serial.println("[WAV] http.begin 失败（URL 无效）");
    return;
  }

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("[WAV] HTTP 状态码 %d，放弃播放\n", status);
    http.end();
    return;
  }

  WiFiClient* stream = http.getStreamPtr();

  WavInfo info;
  if (!parseWavHeader(stream, info)) {
    Serial.println("[WAV] 头解析失败：不是 RIFF/WAVE，或缺少 fmt/data 块");
    http.end();
    return;
  }
  Serial.printf("[WAV] 解析: %luHz / %u 声道 / %ubit / PCM=%u / data=%lu 字节\n",
                (unsigned long)info.sampleRate, info.channels, info.bits,
                info.formatTag, (unsigned long)info.dataSize);

  // 按 WAV 实际采样率配置播放，避免音调/语速不对
  installI2STx(info.sampleRate);
  setRingColor(COLOR_PLAYING);

  static uint8_t raw[WAV_CHUNK_BYTES];
  static int16_t stereoSamples[WAV_CHUNK_BYTES * 2];   // 8bit 单声道时最大需要 2 倍
  uint32_t remaining = info.dataSize;
  uint32_t totalWritten = 0;
  uint16_t failures = 0;
  esp_err_t lastErr = ESP_OK;
  unsigned long tStart = millis();
  bool unsupported = false;

  while (remaining > 0 && failures < 4) {
    size_t want = (remaining < sizeof(raw)) ? (size_t)remaining : sizeof(raw);
    size_t got = readUpTo(stream, raw, want);
    if (got == 0) break;

    // 按完整帧对齐，避免把半帧样本送进 I2S
    size_t frameBytes = (info.bits / 8) * info.channels;
    got -= got % frameBytes;
    if (got == 0) break;
    remaining -= got;

    size_t len = wavToStereo16(raw, got, info, stereoSamples,
                               sizeof(stereoSamples) / sizeof(stereoSamples[0]));
    if (len == 0) {
      Serial.printf("[WAV] 不支持的格式（PCM=%u, %ubit, %u 声道），停止播放\n",
                    info.formatTag, info.bits, info.channels);
      unsupported = true;
      break;
    }

    size_t written = 0;
    esp_err_t e = i2s_write(I2S_PORT, stereoSamples, len, &written, pdMS_TO_TICKS(1000));
    if (e != ESP_OK || written != len) {
      failures++;
      lastErr = e;
      Serial.printf("[WAV] i2s_write 返回 %d (%s)，期望 %u 实际 %u 字节\n",
                    (int)e, esp_err_to_name(e), (unsigned)len, (unsigned)written);
    }
    totalWritten += written;
  }

  if (!unsupported) {
    Serial.printf("[WAV] 播放结束: 写入 %lu 字节 / 用时 %lu ms / 失败 %u 次 / 最后错误 %d (%s)\n",
                  (unsigned long)totalWritten, (unsigned long)(millis() - tStart),
                  failures, (int)lastErr, esp_err_to_name(lastErr));
  }

  http.end();
  setRingColor(COLOR_IDLE);
}

// ---------------- 上传录音并处理 ----------------
void sendRecording() {
  HTTPClient http;
  String endpoint = String(SERVER_URL) + "/api/command";

  if (!http.begin(endpoint)) return;
  http.addHeader("Content-Type", "audio/pcm; rate=16000; channels=1");

  setRingColor(COLOR_THINKING);

  int bytes = http.POST((uint8_t*)pcmBuffer, SAMPLE_COUNT * sizeof(int16_t));

  if (bytes == HTTP_CODE_OK) {
    String response = http.getString();

    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, response) == DeserializationError::Ok) {
      String reply = doc["reply"] | "没有收到回复";
      String ttsUrl = doc["tts_url"] | "";

      Serial.println("识别: " + String(doc["recognized_text"] | ""));
      Serial.println("回复: " + reply);

      if (ttsUrl.length() > 0) {
        playWavFromUrl(ttsUrl);
      }
    }
  } else {
    Serial.printf("[上传] HTTP 状态码 %d（期望 200）\n", bytes);
    ringBlink(COLOR_ERROR, 4, 150);
  }

  http.end();
  setRingColor(COLOR_IDLE);
}

// ---------------- 初始化 ----------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  ring.begin();
  ring.setBrightness(80);
  ring.show();

#if SPEAKER_BEEP_ON_BOOT
  // 硬件自检：开机先“嘟”一声，确认 MAX98357A + 喇叭通路正常（不依赖 Wi-Fi）
  playTestTone(1000, 300, 0.25f);
#endif

  setRingColor(COLOR_THINKING); // 开机联网中
  connectWiFi();

  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());
  Serial.println("串口命令: T = 播放 1kHz 测试音（喇叭自检）");
}

// ---------------- 主循环 ----------------
void loop() {
  // 串口命令：T = 喇叭/功放自检
  if (Serial.available()) {
    int c = Serial.read();
    if (c == 'T' || c == 't') {
      playTestTone(1000, 500, 0.25f);
    }
  }

  // 按下按钮开始一次录音-上传-播放流程
  if (digitalRead(PIN_BUTTON) == LOW) {
    delay(30);
    if (digitalRead(PIN_BUTTON) == LOW) {
      recordPcm();

      if (WiFi.status() == WL_CONNECTED) {
        sendRecording();
      } else {
        Serial.println("Wi-Fi unavailable");
        ringBlink(COLOR_ERROR, 5, 200);
      }

      // 等待按钮松开
      while (digitalRead(PIN_BUTTON) == LOW) {
        delay(10);
      }
    }
  }

  // 慢速呼吸提示待机（可选，占 CPU 很低）
  delay(5);
}
