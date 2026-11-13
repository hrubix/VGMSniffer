#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace scc {

/** CV Hz audio out = hz / kCvHzScale (0..1); Grid multiplies by this to recover Hz. */
inline constexpr float kCvHzScale = 20000.f;

struct SccChannelTuner {
  bool active = false;
  bool gate = false;
  int midiNote = 0;     // 0..127 nearest
  float midiFloat = 0;  // continuous
  float hz = 0;
  float cents = 0;      // vs equal temperament of midiNote
  uint16_t period = 0;
  uint8_t volume = 0; // raw nibble 0=silent .. 15=loud
  bool envelope = false; // PSG hardware envelope on this voice
  float level = 0.f; // CV Vol: SCC vol/16, PSG linear DAC 0..1
  float noiseHz = 0.f;
  char noteName[8]{"—"};
  /** Shared PSG registers. 0 when the file has no PSG. */
  float psgMixer = 0.f;      // (R7 & 0x3F) / 63
  float psgEnvShape = 0.f;   // R13 / 15
  float psgEnvPeriod = 0.f;  // (R11 + R12<<8) / 65535
  uint8_t psgMixerRaw = 0;
  uint8_t psgEnvShapeRaw = 0;
  uint16_t psgEnvPeriodRaw = 0;

  /** CV Vol for the selected voice. Gate off → 0. */
  float loudness() const {
    if (!gate || !active) {
      return 0.f;
    }
    return level;
  }

  /** CV bus level for Hz (0..1). Gate off → 0. */
  float cvHz() const {
    if (!gate || !active || hz <= 0.f) {
      return 0.f;
    }
    const float v = hz / kCvHzScale;
    return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
  }
};

/** One-VGM / one-SCC-channel player for the SCC VGM CLAP. */
class VgmSccEngine {
public:
  VgmSccEngine();
  ~VgmSccEngine();

  VgmSccEngine(const VgmSccEngine&) = delete;
  VgmSccEngine& operator=(const VgmSccEngine&) = delete;

  void prepare(double sampleRate, uint32_t maxBlock);

  bool loadPath(const std::string& path, std::string& error);
  bool loadBytes(const uint8_t* data, size_t size, bool isVgz, const std::string& displayName,
                 std::string& error);
  void unload();

  bool hasFile() const;
  bool isArmed() const; // file + channel 0..7
  bool hasScc() const;
  bool hasPsg() const;
  std::string fileName() const;
  double durationSec() const;
  /** Header has a loop offset and a loop length. Playback returns there after one pass. */
  bool loops() const;

  /** -1 = not selected; 0..4 = SCC; 5..7 = PSG A/B/C. Values 8..10 map back to A/B/C. */
  void setChannel(int channel);
  int channel() const;

  void setStartOffsetSec(double sec);
  double startOffsetSec() const;

  SccChannelTuner tuner() const;
  double playheadSec() const;

  /**
   * Render stereo + optional CV outs for one host block.
   * Pointers may be null. CV values are held across the resampler fraction.
   * Wave × CV Vol equals that source tick (0 when CV Vol is 0).
   * cvNoise is PSG noise pitch / kCvHzScale, else 0.
   * tone and noise are the PSG stems (0 for SCC). Each times CV Vol equals that stem tick.
   * cvMixer, cvEnvShape, and cvEnvPeriod are the shared PSG chip registers (0 without a PSG).
   */
  void process(float* left, float* right, float* cvHz, float* cvVol, float* wave, float* cvNoise,
               float* tone, float* noise, float* cvMixer, float* cvEnvShape, float* cvEnvPeriod,
               uint32_t frames, double songSec, bool playing);

  /** Snapshot for CLAP state (includes VGM bytes). */
  bool saveState(std::vector<uint8_t>& out) const;
  bool loadState(const uint8_t* data, size_t size, std::string& error);

  /** Note tracking deltas since last process (for CLAP note out). */
  struct NoteEvent {
    enum Type { On, Off, Tuning } type = Off;
    int key = 0;
    double tuningCents = 0;
  };
  void drainNoteEvents(std::vector<NoteEvent>& out);

private:
  void applySoloMute();
  void updateTunerFromChip();
  void applyPsgGlobals(SccChannelTuner& tuner);
  void pushNoteTracking();
  void seekToOutput(uint64_t targetOut);
  struct SrcTap {
    int16_t pcm = 0;
    float cvHz = 0.f;
    float cvVol = 0.f;
    float cvNoise = 0.f;
    float wave = 0.f;
    float tone = 0.f;
    float noise = 0.f;
    float cvMixer = 0.f;
    float cvEnvShape = 0.f;
    float cvEnvPeriod = 0.f;
  };

  bool channelUsable(int channel) const;
  int firstVoice() const;
  bool pullSrcSample(SrcTap& tap);
  void renderHostFrames(float* left, float* right, float* cvHz, float* cvVol, float* wave,
                        float* cvNoise, float* tone, float* noise, float* cvMixer, float* cvEnvShape,
                        float* cvEnvPeriod, uint32_t frames);

  mutable std::mutex mu_;
  void* file_ = nullptr;   // VgmFile*
  void* player_ = nullptr; // VgmPlayer*

  double hostRate_ = 44100.0;
  uint32_t maxBlock_ = 512;
  int channel_ = -1;
  double startOffsetSec_ = 0.0;
  double playheadSec_ = 0.0;
  double durationSec_ = 0.0;
  /** Intro length before looping body (header total − loop samples). */
  double loopStartSec_ = 0.0;
  /** Native loop period = duration − loopStart (matches VGM-editor Web Audio). */
  double loopBodySec_ = 0.0;
  uint64_t durationSamples_ = 0;
  uint64_t loopBodySamples_ = 0;
  bool loops_ = false;
  std::string fileName_;
  std::vector<uint8_t> embedded_; // raw file bytes as loaded (may be vgz)
  bool embeddedIsVgz_ = false;
  bool hasScc_ = false;
  bool hasPsg_ = false;

  SccChannelTuner tuner_{};
  bool prevGate_ = false;
  int prevKey_ = -1;
  float prevCents_ = 0;

  std::vector<NoteEvent> noteEvents_;
  std::vector<int16_t> scratch441_;
  /** Fractional offset [0,1) into the 44.1 kHz stream between tapCurr_/tapNext_. */
  double srcFrac_ = 0.0;
  SrcTap tapCurr_{};
  SrcTap tapNext_{};
  bool haveSrcPair_ = false;
  bool streamReady_ = false;
};

} // namespace scc
