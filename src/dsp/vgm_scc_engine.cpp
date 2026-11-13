#include "vgm_scc_engine.h"

extern "C" {
#include "vgmpsg.h"
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <cstdint>

namespace scc {
namespace {

constexpr uint32_t kVgmRate = 44100;
/** Only seek on real transport jumps (~46 ms @ 44.1 kHz), not every block. */
constexpr uint64_t kSeekThresh = 2048;
/** While looping, ignore small forward lag; scrub jumps are larger. */
constexpr uint64_t kLoopForwardSeekThresh = kSeekThresh * 8;

bool nearPeriod(uint64_t dist, uint64_t period, uint64_t tol) {
  if (period == 0 || dist < period - tol) {
    return false;
  }
  const uint64_t rem = dist % period;
  return rem <= tol || period - rem <= tol;
}

double normalizeLoopPlayhead(double sec, double loopStartSec, double loopBodySec) {
  if (!(loopBodySec > 0.0) || !(sec >= loopStartSec)) {
    return sec;
  }
  const double into = std::fmod(sec - loopStartSec, loopBodySec);
  return loopStartSec + (into < 0.0 ? into + loopBodySec : into);
}

void fillNoteName(char out[8], int midiRound) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  const int n = ((midiRound % 12) + 12) % 12;
  const int oct = midiRound / 12 - 1;
  std::snprintf(out, 8, "%s%d", names[n], oct);
}

SccChannelTuner tunerFromHz(double hz, bool audible, uint16_t period, uint8_t vol) {
  SccChannelTuner t;
  t.period = period;
  t.volume = vol;
  t.gate = audible;
  if (!audible || hz <= 0 || !std::isfinite(hz)) {
    std::snprintf(t.noteName, sizeof(t.noteName), "—");
    return t;
  }
  t.active = true;
  t.hz = static_cast<float>(hz);
  const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
  t.midiFloat = static_cast<float>(midi);
  const int midiRound = static_cast<int>(std::lround(midi));
  t.midiNote = std::clamp(midiRound, 0, 127);
  const double refHz = 440.0 * std::pow(2.0, (midiRound - 69) / 12.0);
  t.cents = static_cast<float>(1200.0 * std::log2(hz / refHz));
  fillNoteName(t.noteName, midiRound);
  return t;
}

bool endsWithCi(const std::string& path, const char* suf) {
  const size_t n = std::strlen(suf);
  if (path.size() < n) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    char a = path[path.size() - n + i];
    char b = suf[i];
    if (a >= 'A' && a <= 'Z') {
      a = static_cast<char>(a - 'A' + 'a');
    }
    if (b >= 'A' && b <= 'Z') {
      b = static_cast<char>(b - 'A' + 'a');
    }
    if (a != b) {
      return false;
    }
  }
  return true;
}

std::string baseName(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

void fillConstant(float* buf, uint32_t frames, float value) {
  if (!buf || frames == 0) {
    return;
  }
  std::fill(buf, buf + frames, value);
}

float cvFromHz(double hz) {
  if (!(hz > 0.0) || !std::isfinite(hz)) {
    return 0.f;
  }
  return std::clamp(static_cast<float>(hz / static_cast<double>(kCvHzScale)), 0.f, 1.f);
}

int normalizeChannel(int channel) {
  if (channel >= 8 && channel <= 10) {
    return 5 + (channel - 8);
  }
  return channel;
}

} // namespace

VgmSccEngine::VgmSccEngine() = default;

VgmSccEngine::~VgmSccEngine() { unload(); }

void VgmSccEngine::prepare(double sampleRate, uint32_t maxBlock) {
  std::lock_guard<std::mutex> lock(mu_);
  hostRate_ = sampleRate > 0 ? sampleRate : 44100.0;
  maxBlock_ = maxBlock > 0 ? maxBlock : 512;
  scratch441_.resize(static_cast<size_t>(maxBlock_ * 4 + 64));
}

void VgmSccEngine::unload() {
  std::lock_guard<std::mutex> lock(mu_);
  if (player_) {
    vgm_player_free(static_cast<VgmPlayer*>(player_));
    player_ = nullptr;
  }
  if (file_) {
    vgm_file_free(static_cast<VgmFile*>(file_));
    file_ = nullptr;
  }
  channel_ = -1;
  hasScc_ = false;
  hasPsg_ = false;
  durationSec_ = 0;
  loopStartSec_ = 0;
  loopBodySec_ = 0;
  durationSamples_ = 0;
  loopBodySamples_ = 0;
  loops_ = false;
  playheadSec_ = 0;
  fileName_.clear();
  embedded_.clear();
  tuner_ = {};
  prevGate_ = false;
  prevKey_ = -1;
  streamReady_ = false;
  srcFrac_ = 0.0;
  haveSrcPair_ = false;
  tapCurr_ = {};
  tapNext_ = {};
  noteEvents_.clear();
}

bool VgmSccEngine::hasFile() const {
  std::lock_guard<std::mutex> lock(mu_);
  return file_ != nullptr;
}

bool VgmSccEngine::isArmed() const {
  std::lock_guard<std::mutex> lock(mu_);
  return file_ != nullptr && channelUsable(channel_);
}

bool VgmSccEngine::hasScc() const {
  std::lock_guard<std::mutex> lock(mu_);
  return hasScc_;
}

bool VgmSccEngine::hasPsg() const {
  std::lock_guard<std::mutex> lock(mu_);
  return hasPsg_;
}

bool VgmSccEngine::channelUsable(int channel) const {
  if (channel >= 0 && channel <= 4) {
    return hasScc_;
  }
  if (channel >= 5 && channel <= 7) {
    return hasPsg_;
  }
  return false;
}

int VgmSccEngine::firstVoice() const {
  if (hasScc_) {
    return 0;
  }
  if (hasPsg_) {
    return 5;
  }
  return -1;
}

std::string VgmSccEngine::fileName() const {
  std::lock_guard<std::mutex> lock(mu_);
  return fileName_;
}

double VgmSccEngine::durationSec() const {
  std::lock_guard<std::mutex> lock(mu_);
  return durationSec_;
}

bool VgmSccEngine::loops() const {
  std::lock_guard<std::mutex> lock(mu_);
  return loops_;
}

int VgmSccEngine::channel() const {
  std::lock_guard<std::mutex> lock(mu_);
  return channel_;
}

double VgmSccEngine::startOffsetSec() const {
  std::lock_guard<std::mutex> lock(mu_);
  return startOffsetSec_;
}

SccChannelTuner VgmSccEngine::tuner() const {
  std::lock_guard<std::mutex> lock(mu_);
  return tuner_;
}

double VgmSccEngine::playheadSec() const {
  std::lock_guard<std::mutex> lock(mu_);
  return playheadSec_;
}

void VgmSccEngine::setStartOffsetSec(double sec) {
  std::lock_guard<std::mutex> lock(mu_);
  startOffsetSec_ = std::max(0.0, sec);
  streamReady_ = false;
  srcFrac_ = 0.0;
  haveSrcPair_ = false;
  tapCurr_ = {};
  tapNext_ = {};
}

void VgmSccEngine::applySoloMute() {
  auto* player = static_cast<VgmPlayer*>(player_);
  if (!player || !channelUsable(channel_)) {
    return;
  }
  if (channel_ <= 4) {
    vgm_player_set_mute_mask(player, VGMPSG_MUTE_ALL);
    const uint8_t solo = static_cast<uint8_t>(0x1fu ^ (1u << channel_));
    vgm_player_set_scc_mute_mask(player, solo);
    return;
  }
  const int psg = channel_ - 5;
  vgm_player_set_scc_mute_mask(player, 0x1fu);
  uint8_t mute = VGMPSG_MUTE_ALL;
  mute = static_cast<uint8_t>(mute & ~(VGMPSG_MUTE_TONE_A << psg));
  mute = static_cast<uint8_t>(mute & ~(VGMPSG_MUTE_NOISE_A << psg));
  vgm_player_set_mute_mask(player, mute);
}

void VgmSccEngine::setChannel(int channel) {
  std::lock_guard<std::mutex> lock(mu_);
  channel = normalizeChannel(channel);
  if (channel < -1 || channel > 7) {
    return;
  }
  if (!channelUsable(channel)) {
    channel = firstVoice();
  }
  if (channel_ == channel) {
    return;
  }
  channel_ = channel;
  applySoloMute();
  prevGate_ = false;
  prevKey_ = -1;
  streamReady_ = false;
  srcFrac_ = 0.0;
  haveSrcPair_ = false;
  tapCurr_ = {};
  tapNext_ = {};
  noteEvents_.push_back({NoteEvent::Off, 0, 0});
}

bool VgmSccEngine::loadBytes(const uint8_t* data, size_t size, bool isVgz,
                             const std::string& displayName, std::string& error) {
  if (!data || size == 0) {
    error = "Empty file";
    return false;
  }
  VgmFile* file = vgm_file_load_bytes(data, size, isVgz ? 1 : 0);
  if (!file) {
    error = "Failed to parse VGM";
    return false;
  }
  const bool hasScc = vgm_file_has_scc_data(file) != 0;
  const bool hasPsg = vgm_file_has_psg_data(file) != 0;
  if (!hasScc && !hasPsg) {
    vgm_file_free(file);
    error = "No SCC or PSG data in this VGM";
    return false;
  }
  VgmPlayer* player = vgm_player_create(file);
  if (!player) {
    vgm_file_free(file);
    error = "Failed to create player";
    return false;
  }
  vgm_player_set_mute_mask(player, VGMPSG_MUTE_ALL);
  vgm_player_set_reduce_clicks(player, 0);

  std::lock_guard<std::mutex> lock(mu_);
  if (player_) {
    vgm_player_free(static_cast<VgmPlayer*>(player_));
  }
  if (file_) {
    vgm_file_free(static_cast<VgmFile*>(file_));
  }
  file_ = file;
  player_ = player;
  fileName_ = displayName;
  durationSec_ = vgm_file_duration_sec(file);
  VgmFileInfo info{};
  const bool infoOk = vgm_file_info(file, &info) == 0;
  loops_ = infoOk && info.has_loop && info.loop_offset != 0 && info.loop_samples_44100 > 0;
  durationSamples_ = infoOk ? info.total_samples_44100 : 0;
  // Match VGM-editor _normalizeVgmLoop: period = duration − loopStart.
  if (loops_ && info.total_samples_44100 >= info.loop_samples_44100) {
    const uint32_t intro = info.total_samples_44100 - info.loop_samples_44100;
    loopStartSec_ = static_cast<double>(intro) / static_cast<double>(kVgmRate);
    durationSec_ = static_cast<double>(info.total_samples_44100) / static_cast<double>(kVgmRate);
    loopBodySec_ = std::max(1e-6, durationSec_ - loopStartSec_);
    loopBodySamples_ =
        static_cast<uint64_t>(loopBodySec_ * static_cast<double>(kVgmRate) + 0.5);
    if (loopBodySamples_ == 0) {
      loopBodySamples_ = info.loop_samples_44100;
    }
  } else {
    loopStartSec_ = 0;
    loopBodySec_ = 0;
    loopBodySamples_ = 0;
  }
  embedded_.assign(data, data + size);
  embeddedIsVgz_ = isVgz;
  hasScc_ = hasScc;
  hasPsg_ = hasPsg;
  channel_ = hasScc_ ? 0 : 5;
  playheadSec_ = 0;
  tuner_ = {};
  prevGate_ = false;
  prevKey_ = -1;
  streamReady_ = false;
  srcFrac_ = 0.0;
  haveSrcPair_ = false;
  applySoloMute();
  return true;
}

bool VgmSccEngine::loadPath(const std::string& path, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "Cannot open file";
    return false;
  }
  in.seekg(0, std::ios::end);
  const auto sz = static_cast<size_t>(in.tellg());
  in.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(sz);
  if (sz && !in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz))) {
    error = "Read failed";
    return false;
  }
  const bool vgz = endsWithCi(path, ".vgz");
  return loadBytes(buf.data(), buf.size(), vgz, baseName(path), error);
}

void VgmSccEngine::applyPsgGlobals(SccChannelTuner& tuner) {
  if (!hasPsg_) {
    return;
  }
  auto* player = static_cast<VgmPlayer*>(player_);
  uint8_t mixer6 = 0;
  uint16_t envPeriod = 0;
  uint8_t envShape = 0;
  if (!player || vgm_player_psg_global_regs(player, &mixer6, &envPeriod, &envShape) != 0) {
    return;
  }
  tuner.psgMixerRaw = mixer6;
  tuner.psgEnvShapeRaw = envShape;
  tuner.psgEnvPeriodRaw = envPeriod;
  tuner.psgMixer = static_cast<float>(mixer6) / 63.f;
  tuner.psgEnvShape = static_cast<float>(envShape) / 15.f;
  tuner.psgEnvPeriod = static_cast<float>(envPeriod) / 65535.f;
}

void VgmSccEngine::updateTunerFromChip() {
  auto* player = static_cast<VgmPlayer*>(player_);
  if (!player || !channelUsable(channel_)) {
    tuner_ = {};
    return;
  }
  if (channel_ <= 4) {
    uint16_t period = 0;
    uint8_t vol = 0;
    int enabled = 0;
    if (vgm_player_scc_channel_tap(player, channel_, &period, &vol, &enabled) != 0) {
      tuner_ = {};
      return;
    }
    const uint32_t clk = vgm_player_scc_clock_hz(player);
    const bool audible = enabled && vol > 0 && period > 8;
    const double hz =
        audible ? static_cast<double>(clk) / (32.0 * (static_cast<double>(period) + 1.0)) : 0.0;
    tuner_ = tunerFromHz(hz, audible, period, vol);
    tuner_.level = audible ? static_cast<float>(vol) / 16.f : 0.f;
    applyPsgGlobals(tuner_);
    return;
  }
  const int psg = channel_ - 5;
  uint16_t period = 0;
  int toneOn = 0;
  int noiseOn = 0;
  uint8_t noisePeriod = 0;
  uint8_t vol = 0;
  int envelope = 0;
  float gain = 0.f;
  if (vgm_player_psg_channel_tap(player, psg, &period, &toneOn, &noiseOn, &noisePeriod, &vol,
                                 &envelope, &gain) != 0) {
    tuner_ = {};
    return;
  }
  const uint32_t clk = vgm_player_ay_clock_hz(player);
  const bool tone = toneOn && period > 0;
  const double hz = tone ? static_cast<double>(clk) / (16.0 * static_cast<double>(period)) : 0.0;
  const bool audible = (toneOn || noiseOn) && gain > 0.f;
  tuner_ = tunerFromHz(hz, audible && tone, period, vol);
  if (audible && !tone) {
    tuner_.gate = true;
  }
  tuner_.envelope = envelope != 0;
  tuner_.level = audible ? gain : 0.f;
  if (noiseOn) {
    const int np = noisePeriod == 0 ? 1 : static_cast<int>(noisePeriod);
    tuner_.noiseHz =
        static_cast<float>(static_cast<double>(clk) / (16.0 * static_cast<double>(np)));
  }
  applyPsgGlobals(tuner_);
}

void VgmSccEngine::pushNoteTracking() {
  const bool gate = tuner_.gate && tuner_.active;
  const int key = tuner_.midiNote;
  const float cents = tuner_.cents;
  if (gate && !prevGate_) {
    noteEvents_.push_back({NoteEvent::On, key, cents});
    prevKey_ = key;
    prevCents_ = cents;
  } else if (!gate && prevGate_) {
    noteEvents_.push_back({NoteEvent::Off, prevKey_ >= 0 ? prevKey_ : key, 0});
    prevKey_ = -1;
  } else if (gate && prevGate_) {
    if (key != prevKey_) {
      noteEvents_.push_back({NoteEvent::Off, prevKey_, 0});
      noteEvents_.push_back({NoteEvent::On, key, cents});
      prevKey_ = key;
      prevCents_ = cents;
    } else if (std::fabs(cents - prevCents_) > 0.5f) {
      noteEvents_.push_back({NoteEvent::Tuning, key, cents});
      prevCents_ = cents;
    }
  }
  prevGate_ = gate;
}

void VgmSccEngine::drainNoteEvents(std::vector<NoteEvent>& out) {
  std::lock_guard<std::mutex> lock(mu_);
  out.swap(noteEvents_);
  noteEvents_.clear();
}

void VgmSccEngine::seekToOutput(uint64_t targetOut) {
  auto* player = static_cast<VgmPlayer*>(player_);
  if (!player) {
    return;
  }
  const uint64_t endOut =
      loops_ ? ~uint64_t{0} / 4
             : static_cast<uint64_t>(std::max(0.0, durationSec_) * static_cast<double>(kVgmRate) + 0.5);
  if (vgm_player_seek_output(player, targetOut) != 0) {
    streamReady_ = false;
    return;
  }
  vgm_player_arm_stream(player, endOut > targetOut ? endOut : targetOut + 1);
  streamReady_ = true;
  srcFrac_ = 0.0;
  haveSrcPair_ = false;
  tapCurr_ = {};
  tapNext_ = {};
  // Seek replays chip writes; re-apply solo mute once (not every audio block).
  applySoloMute();
}

bool VgmSccEngine::pullSrcSample(SrcTap& tap) {
  auto* player = static_cast<VgmPlayer*>(player_);
  if (!player || !streamReady_) {
    return false;
  }
  size_t got = 0;
  int16_t sample = 0;
  if (vgm_player_stream_read(player, &sample, 1, &got) != 0 || got == 0) {
    streamReady_ = false;
    return false;
  }
  tap = {};
  tap.pcm = sample;
  if (hasPsg_) {
    uint8_t mixer6 = 0;
    uint16_t envPeriod = 0;
    uint8_t envShape = 0;
    if (vgm_player_psg_global_regs(player, &mixer6, &envPeriod, &envShape) == 0) {
      tap.cvMixer = static_cast<float>(mixer6) / 63.f;
      tap.cvEnvShape = static_cast<float>(envShape) / 15.f;
      tap.cvEnvPeriod = static_cast<float>(envPeriod) / 65535.f;
    }
  }
  if (channel_ <= 4) {
    uint16_t period = 0;
    uint8_t vol = 0;
    int enabled = 0;
    if (vgm_player_scc_channel_tap(player, channel_, &period, &vol, &enabled) == 0) {
      const bool running = enabled && period > 8 && vol > 0;
      if (running) {
        tap.cvVol = static_cast<float>(vol) / 16.f;
        const uint32_t clk = vgm_player_scc_clock_hz(player);
        const double hz =
            static_cast<double>(clk) / (32.0 * (static_cast<double>(period) + 1.0));
        tap.cvHz = cvFromHz(hz);
        if (tap.cvVol > 0.f) {
          tap.wave = (static_cast<float>(sample) / 32768.f) / tap.cvVol;
        }
      }
    }
    return true;
  }
  const int psg = channel_ - 5;
  uint16_t period = 0;
  int toneOn = 0;
  int noiseOn = 0;
  uint8_t noisePeriod = 0;
  uint8_t vol = 0;
  int envelope = 0;
  float gain = 0.f;
  if (vgm_player_psg_channel_tap(player, psg, &period, &toneOn, &noiseOn, &noisePeriod, &vol,
                                 &envelope, &gain) == 0) {
    const uint32_t clk = vgm_player_ay_clock_hz(player);
    if (toneOn && period > 0) {
      tap.cvHz = cvFromHz(static_cast<double>(clk) / (16.0 * static_cast<double>(period)));
    }
    if (noiseOn) {
      const int np = noisePeriod == 0 ? 1 : static_cast<int>(noisePeriod);
      tap.cvNoise = cvFromHz(static_cast<double>(clk) / (16.0 * static_cast<double>(np)));
    }
    if ((toneOn || noiseOn) && gain > 0.f) {
      tap.cvVol = gain;
      tap.wave = (static_cast<float>(sample) / 32768.f) / gain;
      int16_t tonePcm = 0;
      int16_t noisePcm = 0;
      if (vgm_player_psg_stem_pcm(player, psg, &tonePcm, &noisePcm) == 0) {
        if (toneOn) {
          tap.tone = (static_cast<float>(tonePcm) / 32768.f) / gain;
        }
        if (noiseOn) {
          tap.noise = (static_cast<float>(noisePcm) / 32768.f) / gain;
        }
      }
    }
  }
  (void)vol;
  (void)envelope;
  return true;
}

void VgmSccEngine::renderHostFrames(float* left, float* right, float* cvHz, float* cvVol,
                                    float* wave, float* cvNoise, float* tone, float* noise,
                                    float* cvMixer, float* cvEnvShape, float* cvEnvPeriod,
                                    uint32_t frames) {
  auto* player = static_cast<VgmPlayer*>(player_);
  if (!player || !streamReady_ || frames == 0 || !left) {
    if (left) {
      std::fill(left, left + frames, 0.f);
    }
    if (right && right != left) {
      std::fill(right, right + frames, 0.f);
    }
    fillConstant(cvHz, frames, 0.f);
    fillConstant(cvVol, frames, 0.f);
    fillConstant(wave, frames, 0.f);
    fillConstant(cvNoise, frames, 0.f);
    fillConstant(tone, frames, 0.f);
    fillConstant(noise, frames, 0.f);
    fillConstant(cvMixer, frames, 0.f);
    fillConstant(cvEnvShape, frames, 0.f);
    fillConstant(cvEnvPeriod, frames, 0.f);
    return;
  }

  if (!haveSrcPair_) {
    if (!pullSrcSample(tapCurr_) || !pullSrcSample(tapNext_)) {
      std::fill(left, left + frames, 0.f);
      if (right != left) {
        std::fill(right, right + frames, 0.f);
      }
      fillConstant(cvHz, frames, 0.f);
      fillConstant(cvVol, frames, 0.f);
      fillConstant(wave, frames, 0.f);
      fillConstant(cvNoise, frames, 0.f);
      fillConstant(tone, frames, 0.f);
      fillConstant(noise, frames, 0.f);
      fillConstant(cvMixer, frames, 0.f);
      fillConstant(cvEnvShape, frames, 0.f);
      fillConstant(cvEnvPeriod, frames, 0.f);
      return;
    }
    haveSrcPair_ = true;
    srcFrac_ = 0.0;
  }

  const double ratio = static_cast<double>(kVgmRate) / hostRate_;
  for (uint32_t i = 0; i < frames; ++i) {
    while (srcFrac_ >= 1.0) {
      tapCurr_ = tapNext_;
      if (!pullSrcSample(tapNext_)) {
        // Hold last sample for remainder of block if stream ends mid-buffer.
        tapNext_ = tapCurr_;
        srcFrac_ = 0.0;
        break;
      }
      srcFrac_ -= 1.0;
    }
    const float s0 = static_cast<float>(tapCurr_.pcm) / 32768.f;
    const float s1 = static_cast<float>(tapNext_.pcm) / 32768.f;
    const float s = s0 + (s1 - s0) * static_cast<float>(srcFrac_);
    left[i] = s;
    if (right != left) {
      right[i] = s;
    }
    if (cvHz) {
      cvHz[i] = tapCurr_.cvHz;
    }
    if (cvVol) {
      cvVol[i] = tapCurr_.cvVol;
    }
    if (wave) {
      wave[i] = tapCurr_.wave;
    }
    if (cvNoise) {
      cvNoise[i] = tapCurr_.cvNoise;
    }
    if (tone) {
      tone[i] = tapCurr_.tone;
    }
    if (noise) {
      noise[i] = tapCurr_.noise;
    }
    if (cvMixer) {
      cvMixer[i] = tapCurr_.cvMixer;
    }
    if (cvEnvShape) {
      cvEnvShape[i] = tapCurr_.cvEnvShape;
    }
    if (cvEnvPeriod) {
      cvEnvPeriod[i] = tapCurr_.cvEnvPeriod;
    }
    srcFrac_ += ratio;
  }
}

namespace {

void zeroAudio(float* left, float* right, float* cvHz, float* cvVol, float* wave, float* cvNoise,
               float* tone, float* noise, float* cvMixer, float* cvEnvShape, float* cvEnvPeriod,
               uint32_t frames) {
  if (left) {
    std::fill(left, left + frames, 0.f);
  }
  if (right && right != left) {
    std::fill(right, right + frames, 0.f);
  }
  fillConstant(cvHz, frames, 0.f);
  fillConstant(cvVol, frames, 0.f);
  fillConstant(wave, frames, 0.f);
  fillConstant(cvNoise, frames, 0.f);
  fillConstant(tone, frames, 0.f);
  fillConstant(noise, frames, 0.f);
  fillConstant(cvMixer, frames, 0.f);
  fillConstant(cvEnvShape, frames, 0.f);
  fillConstant(cvEnvPeriod, frames, 0.f);
}

} // namespace

void VgmSccEngine::process(float* left, float* right, float* cvHz, float* cvVol, float* wave,
                           float* cvNoise, float* tone, float* noise, float* cvMixer,
                           float* cvEnvShape, float* cvEnvPeriod, uint32_t frames, double songSec,
                           bool playing) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!left) {
    fillConstant(cvHz, frames, 0.f);
    fillConstant(cvVol, frames, 0.f);
    fillConstant(wave, frames, 0.f);
    fillConstant(cvNoise, frames, 0.f);
    fillConstant(tone, frames, 0.f);
    fillConstant(noise, frames, 0.f);
    fillConstant(cvMixer, frames, 0.f);
    fillConstant(cvEnvShape, frames, 0.f);
    fillConstant(cvEnvPeriod, frames, 0.f);
    return;
  }
  if (!file_ || !player_ || !channelUsable(channel_) || !playing) {
    zeroAudio(left, right, cvHz, cvVol, wave, cvNoise, tone, noise, cvMixer, cvEnvShape,
              cvEnvPeriod, frames);
    if (prevGate_) {
      noteEvents_.push_back({NoteEvent::Off, prevKey_ >= 0 ? prevKey_ : 0, 0});
      prevGate_ = false;
      prevKey_ = -1;
    }
    tuner_ = {};
    if (!playing) {
      streamReady_ = false;
      haveSrcPair_ = false;
      srcFrac_ = 0.0;
    }
    playheadSec_ = playing ? playheadSec_ : 0;
    return;
  }

  const double vgmSec = songSec - startOffsetSec_;
  if (vgmSec < 0.0 || (!loops_ && vgmSec >= durationSec_)) {
    playheadSec_ = vgmSec < 0.0 ? 0.0 : vgmSec;
    zeroAudio(left, right, cvHz, cvVol, wave, cvNoise, tone, noise, cvMixer, cvEnvShape, cvEnvPeriod,
              frames);
    if (prevGate_) {
      noteEvents_.push_back({NoteEvent::Off, prevKey_ >= 0 ? prevKey_ : 0, 0});
      prevGate_ = false;
      prevKey_ = -1;
    }
    tuner_ = {};
    streamReady_ = false;
    haveSrcPair_ = false;
    return;
  }

  const uint64_t targetOut =
      static_cast<uint64_t>(vgmSec * static_cast<double>(kVgmRate) + 0.5);
  const uint64_t cur = vgm_player_out_pos(static_cast<VgmPlayer*>(player_));
  // Free-run like VGM-editor's looping BufferSource: the player already wraps at
  // 0x66 with held chip state. Seeking across that seam (host clip wrap ≈ VGM
  // period) rebuilds the chip and clicks. Only seek on real scrubs / re-arm.
  bool needSeek = !streamReady_;
  if (!needSeek) {
    if (targetOut + kSeekThresh < cur) {
      const uint64_t back = cur - targetOut;
      if (loops_ && (nearPeriod(back, loopBodySamples_, kSeekThresh) ||
                     nearPeriod(back, durationSamples_, kSeekThresh))) {
        needSeek = false;
      } else {
        needSeek = true;
      }
    } else if (targetOut > cur + (loops_ ? kLoopForwardSeekThresh : kSeekThresh)) {
      needSeek = true;
    }
  }

  if (needSeek) {
    seekToOutput(targetOut);
    // Time jumped: force a fresh note-on if the channel is already sounding.
    if (prevGate_) {
      noteEvents_.push_back({NoteEvent::Off, prevKey_ >= 0 ? prevKey_ : 0, 0});
    }
    prevGate_ = false;
    prevKey_ = -1;
  }

  const uint64_t outPos = vgm_player_out_pos(static_cast<VgmPlayer*>(player_));
  playheadSec_ = static_cast<double>(outPos) / static_cast<double>(kVgmRate);
  if (loops_) {
    playheadSec_ = normalizeLoopPlayhead(playheadSec_, loopStartSec_, loopBodySec_);
  }

  renderHostFrames(left, right, cvHz, cvVol, wave, cvNoise, tone, noise, cvMixer, cvEnvShape,
                   cvEnvPeriod, frames);
  updateTunerFromChip();
  pushNoteTracking();
}

bool VgmSccEngine::saveState(std::vector<uint8_t>& out) const {
  std::lock_guard<std::mutex> lock(mu_);
  // magic(8) + ver(u32) + channel(i32) + offset(f64) + nameLen(u32) + name + flags(u32) +
  // dataLen(u32) + data
  const char magic[8] = {'S', 'C', 'C', 'V', 'G', 'M', '1', '\0'};
  const uint32_t ver = 1;
  const int32_t ch = channel_;
  const double off = startOffsetSec_;
  const uint32_t nameLen = static_cast<uint32_t>(fileName_.size());
  const uint32_t flags = embeddedIsVgz_ ? 1u : 0u;
  const uint32_t dataLen = static_cast<uint32_t>(embedded_.size());

  out.clear();
  auto append = [&](const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
  };
  append(magic, 8);
  append(&ver, 4);
  append(&ch, 4);
  append(&off, 8);
  append(&nameLen, 4);
  if (nameLen) {
    append(fileName_.data(), nameLen);
  }
  append(&flags, 4);
  append(&dataLen, 4);
  if (dataLen) {
    append(embedded_.data(), dataLen);
  }
  return true;
}

bool VgmSccEngine::loadState(const uint8_t* data, size_t size, std::string& error) {
  if (!data || size < 8 + 4 + 4 + 8 + 4 + 4 + 4) {
    error = "State too small";
    return false;
  }
  if (std::memcmp(data, "SCCVGM1", 7) != 0) {
    error = "Bad state magic";
    return false;
  }
  size_t o = 8;
  auto rd = [&](void* dst, size_t n) -> bool {
    if (o + n > size) {
      return false;
    }
    std::memcpy(dst, data + o, n);
    o += n;
    return true;
  };
  uint32_t ver = 0;
  int32_t ch = -1;
  double off = 0;
  uint32_t nameLen = 0;
  uint32_t flags = 0;
  uint32_t dataLen = 0;
  if (!rd(&ver, 4) || ver != 1 || !rd(&ch, 4) || !rd(&off, 8) || !rd(&nameLen, 4)) {
    error = "Corrupt state header";
    return false;
  }
  std::string name;
  if (nameLen) {
    if (o + nameLen > size) {
      error = "Corrupt name";
      return false;
    }
    name.assign(reinterpret_cast<const char*>(data + o), nameLen);
    o += nameLen;
  }
  if (!rd(&flags, 4) || !rd(&dataLen, 4) || o + dataLen > size) {
    error = "Corrupt payload";
    return false;
  }
  if (dataLen == 0) {
    unload();
    return true;
  }
  if (!loadBytes(data + o, dataLen, (flags & 1u) != 0, name.empty() ? "state.vgm" : name, error)) {
    return false;
  }
  setStartOffsetSec(off);
  if (ch >= 0 && ch <= 10) {
    setChannel(normalizeChannel(ch));
  }
  return true;
}

} // namespace scc
