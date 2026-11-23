// Sound: built-in tunes, RTTTL ringtones, MP3 and WAV files, with volume control.
// Output uses the ESP32's built-in DAC on GPIO26 (the CYD's speaker amplifier) through I2S/DMA.
// Only DAC channel 2 is enabled: GPIO25 (DAC channel 1) is the touch controller's clock line.
// MP3 decoding: minimp3 (public domain, lib/minimp3). RTTTL and WAV are handled here.
#include "app.h"
#include <driver/i2s.h>
#include <driver/rtc_io.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include <minimp3.h>

#define OUT_RATE 44100
#define SPK_PIN 26

// ---------- Built-in tunes (original melodies) ----------
struct BuiltIn { const char* id; const char* label; const char* rtttl; };
static const BuiltIn BUILTINS[] = {
  {"builtin:chime", "Chime (built-in)", "Chime:d=16,o=6,b=140:c,e,g,8c7"},
  {"builtin:fanfare", "Fanfare (built-in)", "Fanfare:d=8,o=5,b=200:g,b,d6,4g6,16p,d6,g6,4c7."},
  {"builtin:island", "Island bells (built-in)",
   "Island:d=8,o=6,b=150:e,g,a,4g,e,d,4e,p,c,d,e,g,4a,g,2e"},
};

const char* soundDir() { return sdOK ? "/countdown/sounds" : "/sounds"; }

String soundLabel(const String& id) {
  for (const BuiltIn& b : BUILTINS)
    if (id == b.id) return b.label;
  int dot = id.lastIndexOf('.');
  return dot > 0 ? id.substring(0, dot) : id;
}

std::vector<String> builtinSounds() {
  std::vector<String> v;
  for (const BuiltIn& b : BUILTINS) v.push_back(b.id);
  return v;
}

// ---------- Sources: each produces mono 16-bit samples at its own rate ----------
class Source {
 public:
  virtual ~Source() {}
  virtual int rate() = 0;
  virtual int read(int16_t* dst, int max) = 0;  // samples written; 0 = finished
};

// RTTTL ringtone -> square wave (exact pitch via a fractional phase accumulator)
class RtttlSource : public Source {
 public:
  explicit RtttlSource(const String& text) : s(text) {
    int c1 = s.indexOf(':'), c2 = c1 >= 0 ? s.indexOf(':', c1 + 1) : -1;
    if (c2 < 0) { pos = s.length(); return; }
    String hdr = s.substring(c1 + 1, c2);
    hdr.replace(" ", "");
    for (String kv : split(hdr)) {
      if (kv.startsWith("d=")) defDur = max(1, (int)kv.substring(2).toInt());
      else if (kv.startsWith("o=")) defOct = constrain((int)kv.substring(2).toInt(), 3, 8);
      else if (kv.startsWith("b=")) bpm = max(10, (int)kv.substring(2).toInt());
    }
    pos = c2 + 1;
  }
  int rate() override { return RATE; }
  int read(int16_t* dst, int max) override {
    int n = 0;
    while (n < max) {
      if (left == 0 && !nextNote()) break;
      int16_t v = 0;
      if (left > gap && inc > 0) {
        v = phase < 0.5f ? AMP : -AMP;
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
      }
      dst[n++] = v;
      left--;
    }
    return n;
  }

 private:
  static const int RATE = 44100, AMP = 24000;
  String s;
  int pos = 0, defDur = 4, defOct = 6, bpm = 63;
  int left = 0, gap = 0;
  float inc = 0, phase = 0;  // cycles per sample, position within the cycle

  static std::vector<String> split(const String& h) {
    std::vector<String> v;
    int a = 0;
    while (a <= (int)h.length()) {
      int b = h.indexOf(',', a);
      if (b < 0) b = h.length();
      v.push_back(h.substring(a, b));
      a = b + 1;
    }
    return v;
  }
  bool num(int& out) {
    int v = 0;
    bool any = false;
    while (pos < (int)s.length() && isdigit((unsigned char)s[pos])) { v = v * 10 + (s[pos++] - '0'); any = true; }
    if (any) out = v;
    return any;
  }
  // Parses e.g. "8c#6." or "4.e" or "p". Returns false at the end of the tune.
  bool nextNote() {
    while (pos < (int)s.length()) {
      while (pos < (int)s.length() && (s[pos] == ',' || isspace((unsigned char)s[pos]))) pos++;
      if (pos >= (int)s.length()) return false;
      int dur = defDur, oct = defOct;
      num(dur);
      bool dotted = false;
      if (pos < (int)s.length() && s[pos] == '.') { dotted = true; pos++; }
      char c = pos < (int)s.length() ? tolower(s[pos++]) : 0;
      static const int8_t SEMI[7] = {9, 11, 0, 2, 4, 5, 7};  // a b c d e f g
      int semi = -1;
      if (c >= 'a' && c <= 'g') semi = SEMI[c - 'a'];
      else if (c == 'h') semi = 11;  // German B
      else if (c != 'p') { while (pos < (int)s.length() && s[pos] != ',') pos++; continue; }  // skip junk
      if (pos < (int)s.length() && s[pos] == '#') { semi++; pos++; }
      if (pos < (int)s.length() && s[pos] == '.') { dotted = true; pos++; }
      num(oct);
      if (pos < (int)s.length() && s[pos] == '.') { dotted = true; pos++; }
      if (dur <= 0) dur = 4;
      float ms = 240000.0f / bpm / dur * (dotted ? 1.5f : 1.0f);
      left = max(1, (int)(ms * RATE / 1000));
      if (semi < 0) {  // rest
        inc = 0;
        gap = left;
      } else {
        float f = 440.0f * powf(2.0f, (semi - 9) / 12.0f + (constrain(oct, 3, 8) - 4));
        inc = min(f / RATE, 0.5f);
        gap = min(left / 10, RATE / 50);  // short silence so repeated notes stay distinct
      }
      phase = 0;
      return true;
    }
    return false;
  }
};

// WAV (PCM 8/16-bit, mono or stereo)
class WavSource : public Source {
 public:
  explicit WavSource(const String& path) {
    f = imgFS->open(path, "r");
    if (!f) return;
    uint8_t h[12];
    if (f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return;
    while (f.available() >= 8) {
      uint8_t ch[8];
      f.read(ch, 8);
      uint32_t len = ch[4] | ch[5] << 8 | ch[6] << 16 | (uint32_t)ch[7] << 24;
      if (!memcmp(ch, "fmt ", 4)) {
        uint8_t fm[16];
        if (len < 16 || f.read(fm, 16) != 16) return;
        int fmt = fm[0] | fm[1] << 8;
        chans = fm[2] | fm[3] << 8;
        hz = fm[4] | fm[5] << 8 | fm[6] << 16 | fm[7] << 24;
        bits = fm[14] | fm[15] << 8;
        if (fmt != 1 || chans < 1 || chans > 2 || (bits != 8 && bits != 16) || hz < 4000) return;
        f.seek(f.position() + len - 16 + (len & 1));
      } else if (!memcmp(ch, "data", 4)) {
        remain = len;
        ok = hz > 0;
        return;
      } else {
        f.seek(f.position() + len + (len & 1));
      }
    }
  }
  ~WavSource() override { if (f) f.close(); }
  bool valid() const { return ok; }
  int rate() override { return hz; }
  int read(int16_t* dst, int max) override {
    if (!ok) return 0;
    int frameBytes = chans * bits / 8, n = 0;
    uint8_t buf[256];
    while (n < max && remain >= (uint32_t)frameBytes) {
      int frames = min(max - n, (int)(sizeof buf / frameBytes));
      frames = min(frames, (int)(remain / frameBytes));
      int got = f.read(buf, frames * frameBytes) / frameBytes;
      if (got <= 0) { remain = 0; break; }
      remain -= got * frameBytes;
      for (int i = 0; i < got; i++) {
        int32_t acc = 0;
        for (int c = 0; c < chans; c++) {
          const uint8_t* p = buf + i * frameBytes + c * bits / 8;
          acc += bits == 16 ? (int16_t)(p[0] | p[1] << 8) : ((int)p[0] - 128) << 8;
        }
        dst[n++] = acc / chans;
      }
    }
    return n;
  }

 private:
  File f;
  bool ok = false;
  int chans = 1, bits = 16, hz = 0;
  uint32_t remain = 0;
};

// MP3 via minimp3
class Mp3Source : public Source {
 public:
  explicit Mp3Source(const String& path) {
    f = imgFS->open(path, "r");
    dec = (mp3dec_t*)malloc(sizeof(mp3dec_t));
    in = (uint8_t*)malloc(IN_SIZE);
    pcm = (int16_t*)malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    if (!f || !dec || !in || !pcm) return;
    skipId3();
    mp3dec_init(dec);
    ok = decodeNext();  // also learns the sample rate
  }
  ~Mp3Source() override {
    if (f) f.close();
    free(dec);
    free(in);
    free(pcm);
  }
  bool valid() const { return ok; }
  int rate() override { return hz; }
  int read(int16_t* dst, int max) override {
    int n = 0;
    while (n < max) {
      if (idx >= count && !decodeNext()) break;
      dst[n++] = pcm[idx++];
    }
    return n;
  }

 private:
  static const int IN_SIZE = 4096;
  File f;
  mp3dec_t* dec = nullptr;
  uint8_t* in = nullptr;
  int16_t* pcm = nullptr;
  int inLen = 0, idx = 0, count = 0, hz = 0;
  bool ok = false, eof = false;

  // Skip an ID3v2 tag (it can hold large album art) so decoding starts at the audio
  void skipId3() {
    uint8_t h[10];
    if (f.read(h, 10) == 10 && !memcmp(h, "ID3", 3)) {
      uint32_t size = ((h[6] & 0x7f) << 21) | ((h[7] & 0x7f) << 14) | ((h[8] & 0x7f) << 7) | (h[9] & 0x7f);
      f.seek(10 + size + ((h[5] & 0x10) ? 10 : 0));
    } else {
      f.seek(0);
    }
  }

  bool decodeNext() {
    for (int tries = 0; tries < 256; tries++) {
      if (!eof && inLen < IN_SIZE) {
        int r = f.read(in + inLen, IN_SIZE - inLen);
        if (r <= 0) eof = true;
        else inLen += r;
      }
      if (inLen == 0) return false;
      mp3dec_frame_info_t info;
      int samples = mp3dec_decode_frame(dec, in, inLen, pcm, &info);
      if (info.frame_bytes == 0) {  // need more data, or nothing decodable is left
        if (eof) return false;
        if (inLen == IN_SIZE) inLen = 0;  // a full buffer of junk: drop it
        continue;
      }
      memmove(in, in + info.frame_bytes, inLen - info.frame_bytes);
      inLen -= info.frame_bytes;
      if (samples == 0) continue;  // ID3 tag or other skipped data
      if (!hz) hz = info.hz;
      if (info.channels == 2)
        for (int i = 0; i < samples; i++) pcm[i] = ((int32_t)pcm[2 * i] + pcm[2 * i + 1]) / 2;
      count = samples;
      idx = 0;
      return true;
    }
    return false;
  }
};

// ---------- DAC output on GPIO26 only ----------
// Between sounds the DAC is switched off and GPIO26 is held at 0 V: a live, idle DAC lets the
// amplifier pick up WiFi noise. Each sound ramps 0 V -> mid-scale -> 0 V so it doesn't pop.
// The I2S clock stays at 44.1 kHz; other rates are converted by repeating or skipping samples.
class DacOut {
 public:
  static void idlePin() {
    pinMode(SPK_PIN, OUTPUT);
    digitalWrite(SPK_PIN, LOW);
  }

  bool open() {
    if (installed) return true;
    i2s_config_t c = {};
    c.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
    c.sample_rate = OUT_RATE;
    c.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    c.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    c.communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_MSB;
    c.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    c.dma_buf_count = 8;
    c.dma_buf_len = 128;
    c.use_apll = false;
    c.tx_desc_auto_clear = false;
    if (i2s_driver_install(I2S_NUM_0, &c, 0, nullptr) != ESP_OK) return false;
    i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);  // GPIO26 only
    installed = true;
    nbuf = 0;
    acc = 0;
    ramp(0, 0x8000);
    return true;
  }

  void close() {
    if (!installed) return;
    flush();
    ramp(0x8000, 0);
    for (int i = 0; i < 8 * 128 + 16; i++) put(0);  // by the time this is queued, the ramp has played
    flush();
    i2s_driver_uninstall(I2S_NUM_0);
    rtc_gpio_deinit((gpio_num_t)SPK_PIN);
    idlePin();
    installed = false;
  }

  void setVolume() {
    float v = constrain(cfg.volume, 0, 100) / 100.0f;
    level = powf(v, 1.6f);  // perceptual curve: more control at the quiet end
  }

  // One mono sample at the source rate -> zero or more output frames at 44.1 kHz
  void sample(int16_t s, int hz) {
    int32_t v = constrain((int32_t)(s * level), -32767, 32767);
    uint16_t u = (uint16_t)(v + 0x8000);
    acc += OUT_RATE;
    while (acc >= (uint32_t)hz) {
      acc -= hz;
      put(u);
    }
  }

  void flush() {
    if (!nbuf) return;
    size_t w;
    i2s_write(I2S_NUM_0, buf, nbuf * 4, &w, portMAX_DELAY);
    nbuf = 0;
  }

 private:
  bool installed = false;
  float level = 0;
  uint32_t acc = 0;
  uint32_t buf[256];
  int nbuf = 0;

  void put(uint16_t u) {
    buf[nbuf++] = (uint32_t)u | ((uint32_t)u << 16);
    if (nbuf == 256) flush();
  }
  // ~50 ms linear ramp between two DAC levels
  void ramp(uint16_t from, uint16_t to) {
    const int n = OUT_RATE / 20;
    for (int i = 0; i <= n; i++) put((uint16_t)(from + ((int32_t)to - from) * i / n));
    flush();
  }
};

static DacOut dac;
static Source* src = nullptr;
static SemaphoreHandle_t audioMux;
static String reqName;
static volatile bool reqPending = false, reqStop = false, playing = false;

static String ext(const String& n) {
  String l = n;
  l.toLowerCase();
  int dot = l.lastIndexOf('.');
  return dot > 0 ? l.substring(dot + 1) : String("");
}

static Source* makeSource(const String& name) {
  for (const BuiltIn& b : BUILTINS)
    if (name == b.id) return new RtttlSource(b.rtttl);
  String path = String(soundDir()) + "/" + name;
  if (!imgFS->exists(path)) return nullptr;
  String e = ext(name);
  if (e == "mp3") {
    Mp3Source* m = new Mp3Source(path);
    if (m->valid()) return m;
    delete m;
  } else if (e == "wav") {
    WavSource* w = new WavSource(path);
    if (w->valid()) return w;
    delete w;
  } else if (e == "rtttl" || e == "txt") {
    File f = imgFS->open(path, "r");
    if (!f) return nullptr;
    String t = f.readString();
    f.close();
    if (t.length() > 4000) t = t.substring(0, 4000);
    return new RtttlSource(t);
  }
  return nullptr;
}

static void finish() {
  if (src) {
    delete src;
    src = nullptr;
  }
  dac.close();
  playing = false;
}

static TaskHandle_t audioTaskH = nullptr;
static const uint32_t AUDIO_STACK = 28672;  // minimp3 keeps ~16 KB of scratch space on the stack

// The audio task (and its 28 KB stack) only exists while sounds are playing; it ends itself
// after a few idle seconds so that memory is free for the weather requests and web server.
static void audioTask(void*) {
  static int16_t block[256];
  uint32_t started = 0, idleSince = millis();
  for (;;) {
    if (reqStop || reqPending) {
      if (src) finish();
      reqStop = false;
    }
    if (reqPending) {
      xSemaphoreTake(audioMux, portMAX_DELAY);
      String n = reqName;
      reqPending = false;
      xSemaphoreGive(audioMux);
      src = makeSource(n);
      if (src && src->rate() > 0 && dac.open()) {
        playing = true;
        started = millis();
      } else {
        Serial.printf("Couldn't play sound '%s'\n", n.c_str());
        finish();
      }
    }
    if (src) {
      dac.setVolume();
      int n = src->read(block, 256);
      int hz = src->rate();
      for (int i = 0; i < n; i++) dac.sample(block[i], hz);  // blocks while the DMA buffer is full
      // Stop at the end, or after 90 s: long songs shouldn't run on forever
      if (n == 0 || millis() - started > 90000) finish();
      idleSince = millis();
    } else {
      if (millis() - idleSince > 3000) {
        xSemaphoreTake(audioMux, portMAX_DELAY);
        if (!reqPending) {
          audioTaskH = nullptr;
          reqStop = false;
          xSemaphoreGive(audioMux);
          vTaskDelete(nullptr);
        }
        xSemaphoreGive(audioMux);
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

void audioBegin() {
  DacOut::idlePin();
  audioMux = xSemaphoreCreateMutex();
}

void audioPlay(const String& name) {
  xSemaphoreTake(audioMux, portMAX_DELAY);
  reqName = name;
  reqPending = true;
  if (!audioTaskH &&
      xTaskCreatePinnedToCore(audioTask, "audio", AUDIO_STACK, nullptr, 2, &audioTaskH, 0) != pdPASS) {
    audioTaskH = nullptr;
    reqPending = false;
    Serial.println("Not enough memory to start audio");
  }
  xSemaphoreGive(audioMux);
}

void audioStop() {
  if (audioTaskH) reqStop = true;
}
bool audioPlaying() { return playing || reqPending; }

// which: 0 = milestone, 1 = arrival celebration
void audioEvent(int which, bool force) {
  if (!force && (!cfg.sound || cfg.volume == 0 || inQuietHours())) return;
  audioPlay(which == 1 ? cfg.soundArr : cfg.soundMile);
}
