

#include <WiFi.h>
#include <SPIFFS.h>
#include <driver/i2s.h>
#include <string.h>
#include "esp_heap_caps.h"

/* ===== Wi-Fi ===== */
#define WIFI_SSID "m"
#define WIFI_PASS "12341234"

/* ===== Server ===== */
const char* SERVER_BASE = "http://192.168.229.121:8000";
const char* URL_UPLOAD  = "/upload-audio/";

#define WIFI_LED   21
#define TRANS_LED  19
#define BTN_PIN    33   

// I2S - INMP441
#define I2S_WS   15
#define I2S_SD   13
#define I2S_SCK  2

// I2S - MAX98357A
#define I2S_DOUT 25
#define I2S_BCLK 26
#define I2S_LRC  22
#define AMP_SHDN_PIN  17              // تعطيل/تمكين المضخِّم (HIGH للتشغيل)


/* ===== Recording (5s WAV 16kHz mono 16-bit) ===== */
#define REC_PORT           I2S_NUM_0
#define REC_SAMPLE_RATE    16000
#define REC_BITS           I2S_BITS_PER_SAMPLE_16BIT
#define REC_READ_LEN       (4 * 1024)   // 4096: DMA-friendly وبيخفف ضغط الهيب
#define REC_SECS           5
#define WAV_HEADER_SIZE    44
#define REC_CHANNELS       1
#define REC_BYTES_TOTAL    (REC_CHANNELS * REC_SAMPLE_RATE * 2 * REC_SECS)

/* ===== Playback ===== */
#define PB_PORT            I2S_NUM_1
#define PB_BITS            I2S_BITS_PER_SAMPLE_16BIT
#define PB_IN_CHUNK        1024
#define PB_OUT_CHUNK       (PB_IN_CHUNK * 4)  // أسوأ حالة (24k->48k ستيريو)
#define BOOST_GAIN_SHIFT   1
#define UPSAMPLE_24K_TO_48K 1

const char* recPath = "/recording.wav";

const uint16_t DEBOUNCE_MS = 30;
bool lastStable = false, lastRead = false;
unsigned long lastChangeMs = 0;

bool isRecording = false;
static bool pbInstalled = false;

static uint8_t* gInBuf  = nullptr;   // PB_IN_CHUNK
static uint8_t* gOutBuf = nullptr;   // PB_OUT_CHUNK
static char*    recI2SBuf   = nullptr;   // REC_READ_LEN
static uint8_t* recFlashBuf = nullptr;   // REC_READ_LEN

void i2sInitMic();
void i2sDeinitMic();
void i2sInitDAC(uint32_t sampleRate);
void i2sDeinitDAC();
bool recordToSPIFFS();
void wavHeader(byte *h, uint32_t dataBytes);
static void scaleI2S(uint8_t *dst, uint8_t *src, uint32_t len);
bool postMultipartAndPlay(const char* path);
bool parseServerBase(const char* base, String& host, uint16_t& port);
bool playWavFromHTTPBody(WiFiClient& client, bool isChunked);

static void* malloc_dma8(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
  if (!p) p = malloc(n);
  return p;
}

static bool waitForData(WiFiClient& c, uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (!c.available() && c.connected() && (millis() - t0 < timeoutMs)) { delay(10); yield(); }
  return c.available();
}

static void printMem(const char* tag) {
  Serial.printf("[MEM] %s: freeHeap=%u, maxAlloc=%u\n", tag, (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void setup() {
  Serial.begin(115200);
  pinMode(WIFI_LED, OUTPUT);  digitalWrite(WIFI_LED, LOW);
  pinMode(TRANS_LED, OUTPUT); digitalWrite(TRANS_LED, LOW);
  pinMode(BTN_PIN, INPUT_PULLDOWN); 
  pinMode(AMP_SHDN_PIN, OUTPUT);                                  // دبوس تعطيل/تمكين الأمب
  digitalWrite(AMP_SHDN_PIN, LOW);   
  Serial.println("\nESP32 voice (hardened): record 5s -> upload -> play WAV.");
  printMem("boot");

  if (!SPIFFS.begin(true)) {
    Serial.println("[FS] SPIFFS init FAILED");
    while (1) { delay(500); }
  }

  gInBuf  = (uint8_t*)malloc_dma8(PB_IN_CHUNK);
  gOutBuf = (uint8_t*)malloc_dma8(PB_OUT_CHUNK);
  if (!gInBuf || !gOutBuf) {
    Serial.println("[MEM] Playback buffers alloc FAILED");
    while (1) { delay(500); }
  }

  recI2SBuf   = (char*)   malloc_dma8(REC_READ_LEN);
  recFlashBuf = (uint8_t*)malloc_dma8(REC_READ_LEN);
  if (!recI2SBuf || !recFlashBuf) {
    Serial.println("[MEM] Record buffers alloc FAILED");
    while (1) { delay(500); }
  }

  printMem("after malloc");

  // Wi-Fi
  Serial.printf("Connecting WiFi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    digitalWrite(WIFI_LED, HIGH); delay(180);
    digitalWrite(WIFI_LED, LOW);  delay(180);
    Serial.print(".");
    yield();
  }
  Serial.println();
  digitalWrite(WIFI_LED, (WiFi.status()==WL_CONNECTED)?HIGH:LOW);

  i2sInitMic();
  i2s_zero_dma_buffer(REC_PORT);
  printMem("ready");
}

void loop() {
  digitalWrite(WIFI_LED, (WiFi.status() == WL_CONNECTED) ? HIGH : LOW);

  bool raw = digitalRead(BTN_PIN);
  bool logicalPressed = (raw == HIGH);

  if (logicalPressed != lastRead) { lastRead = logicalPressed; lastChangeMs = millis(); }

  if ((millis() - lastChangeMs) >= DEBOUNCE_MS) {
    if (lastStable != logicalPressed) {
      lastStable = logicalPressed;

      if (lastStable && !isRecording) {
        Serial.println("Pressed -> Start 5s recording...");
        isRecording = true;
        digitalWrite(TRANS_LED, HIGH);

        i2sDeinitDAC();
        delay(5);
        i2s_zero_dma_buffer(REC_PORT);

        bool ok = recordToSPIFFS();
        digitalWrite(TRANS_LED, LOW);
        isRecording = false;

        if (!ok) {
          Serial.println("[REC] Recording failed.");
          return;
        }

        File f = SPIFFS.open(recPath, FILE_READ);
        if (!f) { Serial.println("[REC] File not found!"); return; }
        Serial.printf("[REC] File %s size = %d bytes (expected ~%u)\n",
                      recPath, (int)f.size(), (unsigned)(WAV_HEADER_SIZE + REC_BYTES_TOTAL));
        f.close();

        if (WiFi.status() != WL_CONNECTED) { Serial.println("[NET] WiFi not connected."); return; }

        Serial.println("[NET] Uploading (multipart) and expecting WAV in the same response...");
        digitalWrite(TRANS_LED, HIGH);
        bool ok2 = postMultipartAndPlay(recPath);
        digitalWrite(TRANS_LED, LOW);
        Serial.println(ok2 ? "[PLAY] Finished." : "[PLAY] Failed.");
      } else if (!lastStable) {
        Serial.println("Released");
      }
    }
  }

  delay(5);
  yield();
}

void i2sInitMic() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = REC_SAMPLE_RATE;
  cfg.bits_per_sample = REC_BITS;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 32;   // أقل من 64 لتخفيف استهلاك الذاكرة
  cfg.dma_buf_len = 256;    // مجموع DMA = 32*256*2 بايت ≈ 16KB
  cfg.use_apll = true;
  cfg.tx_desc_auto_clear = false;
  cfg.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num   = I2S_SCK;
  pins.ws_io_num    = I2S_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = I2S_SD;
#if defined(I2S_PIN_NO_CHANGE)
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
#endif

  esp_err_t e;
  e = i2s_driver_install(REC_PORT, &cfg, 0, NULL);
  if (e != ESP_OK) { Serial.printf("[I2S RX] install failed: %d\n", e); while(1){delay(500);} }
  e = i2s_set_pin(REC_PORT, &pins);
  if (e != ESP_OK) { Serial.printf("[I2S RX] set_pin failed: %d\n", e); while(1){delay(500);} }
  i2s_zero_dma_buffer(REC_PORT);
}

void i2sDeinitMic() { i2s_driver_uninstall(REC_PORT); }

void i2sInitDAC(uint32_t sampleRate) {
  if (sampleRate < 8000) sampleRate = 8000;
  if (sampleRate > 96000) sampleRate = 96000;

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = sampleRate;
  cfg.bits_per_sample = PB_BITS;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 12;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num   = I2S_BCLK;
  pins.ws_io_num    = I2S_LRC;
  pins.data_out_num = I2S_DOUT;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;
#if defined(I2S_PIN_NO_CHANGE)
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
#endif

  if (!pbInstalled) {
    esp_err_t e = i2s_driver_install(PB_PORT, &cfg, 0, NULL);
    if (e != ESP_OK) { Serial.printf("[I2S TX] install failed: %d\n", e); return; }
    i2s_set_pin(PB_PORT, &pins);
    pbInstalled = true;
  } else {
    i2s_set_pin(PB_PORT, &pins);
  }
  i2s_zero_dma_buffer(PB_PORT);
  i2s_set_clk(PB_PORT, sampleRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
}

void i2sDeinitDAC() {
  if (pbInstalled) {
    i2s_driver_uninstall(PB_PORT);
    pbInstalled = false;
  }
}

void wavHeader(byte *h, uint32_t dataBytes) {
  uint32_t chunkSize = 36 + dataBytes;
  uint16_t audioFmt  = 1, chans = 1, bits = 16;
  uint32_t byteRate  = REC_SAMPLE_RATE * chans * (bits/8);
  uint16_t blockAlgn = chans * (bits/8);

  h[0]='R';h[1]='I';h[2]='F';h[3]='F';
  h[4]= chunkSize &0xFF; h[5]=(chunkSize>>8)&0xFF; h[6]=(chunkSize>>16)&0xFF; h[7]=(chunkSize>>24)&0xFF;
  h[8]='W';h[9]='A';h[10]='V';h[11]='E';
  h[12]='f';h[13]='m';h[14]='t';h[15]=' ';
  h[16]=16;h[17]=0;h[18]=0;h[19]=0;
  h[20]= audioFmt &0xFF; h[21]=(audioFmt>>8)&0xFF;
  h[22]= chans &0xFF; h[23]=(chans>>8)&0xFF;
  h[24]= REC_SAMPLE_RATE &0xFF; h[25]=(REC_SAMPLE_RATE>>8)&0xFF; h[26]=(REC_SAMPLE_RATE>>16)&0xFF; h[27]=(REC_SAMPLE_RATE>>24)&0xFF;
  h[28]= byteRate &0xFF; h[29]=(byteRate>>8)&0xFF; h[30]=(byteRate>>16)&0xFF; h[31]=(byteRate>>24)&0xFF;
  h[32]= blockAlgn &0xFF; h[33]=(blockAlgn>>8)&0xFF;
  h[34]= bits &0xFF; h[35]=(bits>>8)&0xFF;
  h[36]='d';h[37]='a';h[38]='t';h[39]='a';
  h[40]= dataBytes &0xFF; h[41]=(dataBytes>>8)&0xFF; h[42]=(dataBytes>>16)&0xFF; h[43]=(dataBytes>>24)&0xFF;
}

static void scaleI2S(uint8_t *dst, uint8_t *src, uint32_t len) {
  uint32_t j = 0; uint32_t v = 0;
  for (uint32_t i=0; i+1<len; i+=2) {
    v = ((((uint16_t)(src[i+1] & 0x0F) << 8) | (src[i])));
    dst[j++] = 0;
    dst[j++] = v * 256 / 2048;
  }
}

bool recordToSPIFFS() {
  if (!recI2SBuf || !recFlashBuf) {
    Serial.println("[REC] buffers not ready");
    return false;
  }

  if (SPIFFS.exists(recPath)) SPIFFS.remove(recPath);
  File f = SPIFFS.open(recPath, FILE_WRITE);
  if (!f) { Serial.println("[REC] open file failed"); return false; }

  byte header[WAV_HEADER_SIZE];
  wavHeader(header, REC_BYTES_TOTAL);
  if (f.write(header, WAV_HEADER_SIZE) != WAV_HEADER_SIZE) { Serial.println("[REC] header write fail"); f.close(); return false; }

  i2s_zero_dma_buffer(REC_PORT);
  delay(5);

  size_t br = 0;

  if (i2s_read(REC_PORT, recI2SBuf, REC_READ_LEN, &br, 2000) != ESP_OK) { Serial.println("[REC] warmup read fail"); f.close(); return false; }
  if (i2s_read(REC_PORT, recI2SBuf, REC_READ_LEN, &br, 2000) != ESP_OK) { Serial.println("[REC] warmup read2 fail"); f.close(); return false; }

  Serial.println("[REC] Recording 5s...");
  int written = 0;
  while (written < REC_BYTES_TOTAL) {
    int remaining = REC_BYTES_TOTAL - written;
    int toProcess = min(remaining, (int)REC_READ_LEN);

    esp_err_t e = i2s_read(REC_PORT, recI2SBuf, REC_READ_LEN, &br, 50 /*ticks*/);
    if (e != ESP_OK || br == 0) {
      e = i2s_read(REC_PORT, recI2SBuf, REC_READ_LEN, &br, 2000);
      if (e != ESP_OK || br == 0) { Serial.printf("[REC] i2s_read fail e=%d br=%u\n", e, (unsigned)br); f.close(); return false; }
    }

    if ((int)br < toProcess) toProcess = (int)br;
    if (toProcess <= 0) { Serial.println("[REC] toProcess<=0"); f.close(); return false; }

    scaleI2S(recFlashBuf, (uint8_t*)recI2SBuf, toProcess);

    size_t w = f.write(recFlashBuf, toProcess);
    if ((int)w != toProcess) { Serial.printf("[REC] write fail w=%u\n", (unsigned)w); f.close(); return false; }

    written += toProcess;
    int pct = (written * 100) / REC_BYTES_TOTAL;
    if (pct > 100) pct = 100;
    Serial.printf("[REC] %d%%\n", pct);

    yield();
  }

  f.close();
  Serial.println("[REC] Finished (exact size).");
  return true;
}

bool postMultipartAndPlay(const char* path) {
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) { Serial.println("[NET] open file failed"); return false; }

  String host; uint16_t port;
  if (!parseServerBase(SERVER_BASE, host, port)) { Serial.println("[NET] SERVER_BASE parse failed"); f.close(); return false; }

  String boundary = "----ESP32FormBoundary7MA4YWxkTrZu0gW";
  String head = "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"audio\"; filename=\"recording.wav\"\r\n"
                "Content-Type: audio/wav\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  size_t contentLength = head.length() + f.size() + tail.length();
  String pathOnly = String(URL_UPLOAD);

  WiFiClient client;
  client.setTimeout(60000);
  if (!client.connect(host.c_str(), port)) { Serial.println("[NET] TCP connect failed"); f.close(); return false; }

  client.printf("POST %s HTTP/1.1\r\n", pathOnly.c_str());
  client.printf("Host: %s:%u\r\n", host.c_str(), port);
  client.print("Connection: close\r\n");
  client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary.c_str());
  client.printf("Content-Length: %u\r\n", (unsigned)contentLength);
  client.print("Accept: */*\r\n");
  client.print("X-Return-Audio: 1\r\n");
  client.print("User-Agent: ESP32-Audio/1.0\r\n");
  client.print("\r\n");

  client.print(head);
  uint8_t buf[1024];
  while (f.available()) {
    int n = f.read(buf, sizeof(buf));
    if (n > 0) client.write(buf, n);
    yield();
  }
  f.close();
  client.print(tail);

  if (!waitForData(client, 60000)) {
    Serial.println("[NET] No response within timeout");
    client.stop();
    return false;
  }

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  Serial.println("[NET] Resp: " + statusLine);
  if (!(statusLine.startsWith("HTTP/1.1 200") || statusLine.startsWith("HTTP/1.0 200"))) {
    for (int hdr=0; hdr<30 && client.connected(); ++hdr) {
      String line = client.readStringUntil('\n');
      if (line == "\r" || line.length() == 0) break;
      Serial.print("[NET] H: "); Serial.print(line);
    }
    client.stop();
    return false;
  }

  bool isChunked = false;
  bool isWav = false;
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;
    String low = line; low.toLowerCase();
    if (low.startsWith("content-type:")) {
      if (low.indexOf("audio/wav") >= 0 || low.indexOf("audio/x-wav") >= 0) isWav = true;
    }
    if (low.startsWith("transfer-encoding:") && low.indexOf("chunked") >= 0) isChunked = true;
  }
  if (!isWav) {
    Serial.println("[WAV] Server did not return audio/wav (maybe JSON).");
    client.stop();
    return false;
  }

  bool ok = playWavFromHTTPBody(client, isChunked);
  client.stop();
  return ok;
}

bool parseServerBase(const char* base, String& host, uint16_t& port) {
  String s = String(base);
  if (s.startsWith("http://")) s.remove(0, 7);
  int slash = s.indexOf('/');
  if (slash >= 0) s = s.substring(0, slash);
  int colon = s.indexOf(':');
  if (colon < 0) { host = s; port = 80; return host.length() > 0; }
  host = s.substring(0, colon);
  port = (uint16_t)s.substring(colon + 1).toInt();
  if (port == 0) port = 80;
  return host.length() > 0;
}

bool playWavFromHTTPBody(WiFiClient& stream, bool isChunked) {
  auto readNRaw = [&](uint8_t* buf, size_t n) -> bool {
    size_t got = 0;
    while (got < n) {
      int r = stream.readBytes(buf + got, n - got);
      if (r <= 0) return false;
      got += r;
      yield();
    }
    return true;
  };

  // read a line from stream (up to '\n'), return false if timeout/closed
  auto readLine = [&](String &out, uint32_t timeoutMs=30000)->bool {
    out = "";
    uint32_t t0 = millis();
    while (true) {
      while (stream.available()) {
        char c = stream.read();
        out += c;
        if (c == '\n') return true;
      }
      if (!stream.connected() || (millis() - t0) > timeoutMs) return false;
      delay(1); yield();
    }
    return false;
  };

  // helper to read exactly n bytes, handling chunked if needed
  auto readBodyBytes = [&](uint8_t* buf, size_t n, bool &ok) -> size_t {
    ok = true;
    if (!isChunked) {
      if (!readNRaw(buf, n)) { ok = false; return 0; }
      return n;
    }
    // chunked: read hex-size line, then that many bytes, repeat as needed
    size_t totalGot = 0;
    while (totalGot < n) {
      // if current chunk buffer is empty, read next chunk header
      String line;
      if (!readLine(line, 30000)) { ok = false; return totalGot; }
      line.trim();
      if (line.length() == 0) {
        // ignore empty lines
        continue;
      }
      // parse hex chunk size
      long chunkSize = strtol(line.c_str(), NULL, 16);
      if (chunkSize == 0) { ok = false; return totalGot; } // no more data
      size_t toRead = (size_t)min<long>((long)chunkSize, (long)(n - totalGot));
      if (!readNRaw(buf + totalGot, toRead)) { ok = false; return totalGot; }
      totalGot += toRead;

      // consume the trailing CRLF after chunk data (if we didn't read entire chunk we still must skip remainder)
      size_t remainingInChunk = chunkSize - toRead;
      while (remainingInChunk > 0) {
        uint8_t skipBuf[128];
        size_t r = min<size_t>(remainingInChunk, sizeof(skipBuf));
        if (!readNRaw(skipBuf, r)) { ok = false; return totalGot; }
        remainingInChunk -= r;
      }
      // now read and discard the CRLF after the chunk
      String crlf;
      if (!readLine(crlf, 2000)) { ok = false; return totalGot; } // this reads up to \n including CRLF
    }
    return totalGot;
  };

  // read first 12 bytes of RIFF header (RIFF + size + WAVE)
  uint8_t hdr[12];
  bool ok;
  if (!readNRaw(hdr, 12)) { Serial.println("[WAV] header too short"); return false; }
  if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) { Serial.println("[WAV] Not RIFF/WAVE"); return false; }

  uint32_t sampleRate = 16000;
  uint16_t bitsPerSample = 16;
  uint16_t numChannels = 1;
  bool fmtSeen = false;

  while (stream.connected()) {
    // read chunk header (8 bytes) - but use readBodyBytes which handles chunked on top
    uint8_t ch[8];
    size_t got = 0;
    // read the 8 bytes of chunk header possibly across chunk boundaries
    while (got < 8) {
      size_t need = 8 - got;
      size_t read = 0;
      read = readBodyBytes(ch + got, need, ok);
      if (!ok || read == 0) { Serial.println("[WAV] chunk hdr short"); return false; }
      got += read;
    }

    char id[5] = { (char)ch[0], (char)ch[1], (char)ch[2], (char)ch[3], 0 };
    uint32_t chunkSize = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) | ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);

    if (!strcmp(id, "fmt ")) {
      if (chunkSize < 16) { Serial.println("[WAV] fmt too small"); return false; }
      uint8_t fmtBuf[40];
      size_t toRead = min<uint32_t>(chunkSize, sizeof(fmtBuf));
      if (readBodyBytes(fmtBuf, toRead, ok) != toRead || !ok) { Serial.println("[WAV] fmt read fail"); return false; }
      // skip remainder in fmt if any
      uint32_t remain = (chunkSize > toRead) ? (chunkSize - toRead) : 0;
      while (remain) {
        uint8_t sink[128];
        size_t r = min<size_t>(remain, sizeof(sink));
        if (readBodyBytes(sink, r, ok) != r || !ok) return false;
        remain -= r;
      }

      uint16_t audioFmt = (uint16_t)(fmtBuf[0] | (fmtBuf[1]<<8));
      numChannels      = (uint16_t)(fmtBuf[2] | (fmtBuf[3]<<8));
      sampleRate       = (uint32_t)(fmtBuf[4] | (fmtBuf[5]<<8) | (fmtBuf[6]<<16) | (fmtBuf[7]<<24));
      bitsPerSample    = (uint16_t)(fmtBuf[14] | (fmtBuf[15]<<8));

      Serial.printf("[WAV] fmt: fmt=%u ch=%u sr=%u bits=%u\n", audioFmt, numChannels, sampleRate, bitsPerSample);
      if (audioFmt != 1 || bitsPerSample != 16 || (numChannels != 1 && numChannels != 2)) {
        Serial.println("[WAV] unsupported wav format"); return false;
      }

      uint32_t playRate = sampleRate;
  #if UPSAMPLE_24K_TO_48K
      if (sampleRate == 24000) playRate = 48000;
  #endif
      digitalWrite(AMP_SHDN_PIN, HIGH);   // ensure amp enabled
      i2sInitDAC(playRate);
      fmtSeen = true;
    }
    else if (!strcmp(id, "data")) {
      if (!fmtSeen) { Serial.println("[WAV] data before fmt"); return false; }
      Serial.printf("[WAV] data size=%u, sr=%u, ch=%u\n", chunkSize, sampleRate, numChannels);

      bool sizeUnknown = (chunkSize == 0xFFFFFFFF);
      uint32_t remaining = chunkSize;
      size_t totalWritten = 0;

      while (stream.connected()) {
        int toRead = PB_IN_CHUNK;
        if (!sizeUnknown) {
          if (remaining == 0) break;
          toRead = (int)min<uint32_t>(PB_IN_CHUNK, remaining);
        }

        int r = (int)readBodyBytes(gInBuf, toRead, ok);
        if (!ok || r <= 0) break;
        if (!sizeUnknown) remaining -= r;

        // convert mono or pass-through stereo
        if (numChannels == 1) {
          int outIdx = 0;
          for (int i = 0; i + 1 < r; i += 2) {
            int16_t s = (int16_t)((gInBuf[i+1] << 8) | gInBuf[i]);

  #if BOOST_GAIN_SHIFT > 0
            int32_t tmp = (int32_t)s << BOOST_GAIN_SHIFT;
            if (tmp > 32767) tmp = 32767;
            if (tmp < -32768) tmp = -32768;
            s = (int16_t)tmp;
  #endif

  #if UPSAMPLE_24K_TO_48K
            if (sampleRate == 24000) {
              // duplicate samples to upsample 2x (24->48)
              gOutBuf[outIdx++] = (uint8_t)(s & 0xFF);
              gOutBuf[outIdx++] = (uint8_t)((s >> 8) & 0xFF);
              gOutBuf[outIdx++] = (uint8_t)(s & 0xFF);
              gOutBuf[outIdx++] = (uint8_t)((s >> 8) & 0xFF);
              // and again to make stereo if needed
              gOutBuf[outIdx++] = (uint8_t)(s & 0xFF);
              gOutBuf[outIdx++] = (uint8_t)((s >> 8) & 0xFF);
              gOutBuf[outIdx++] = (uint8_t)(s & 0xFF);
              gOutBuf[outIdx++] = (uint8_t)((s >> 8) & 0xFF);
            } else
  #endif
            {
              // make stereo by duplicating sample (L and R)
              gOutBuf[outIdx++] = (uint8_t)(s & 0xFF);
              gOutBuf[outIdx++] = (uint8_t)((s >> 8) & 0xFF);
              gOutBuf[outIdx++] = (uint8_t)(s & 0xFF);
              gOutBuf[outIdx++] = (uint8_t)((s >> 8) & 0xFF);
            }
          }
          size_t written = 0;
          esp_err_t wret = i2s_write(PB_PORT, gOutBuf, outIdx, &written, portMAX_DELAY);
          totalWritten += written;
        } else {
          size_t written = 0;
          esp_err_t wret = i2s_write(PB_PORT, gInBuf, r, &written, portMAX_DELAY);
          totalWritten += written;
        }

        yield();
      }

      Serial.printf("[WAV] bytes to I2S = %u\n", (unsigned)totalWritten);
      i2sDeinitDAC();
      return (totalWritten > 0);
    }
    else {
      // skip unknown chunk
      uint32_t remain = chunkSize;
      while (remain) {
        uint8_t sink[128];
        size_t r = min<size_t>(remain, sizeof(sink));
        if (readBodyBytes(sink, r, ok) != r || !ok) return false;
        remain -= r;
        yield();
      }
    }
  }
  return false;
}
