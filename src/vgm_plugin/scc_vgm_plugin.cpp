#include "scc_vgm_plugin.h"

#include "scc_vgm_editor.h"
#include "scc_vgm_version.h"
#include "vgm_file_dialog.h"

#include <clap/helpers/host-proxy.hxx>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace scc {

namespace {
const char* features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_NOTE_DETECTOR, "vgm",
                          "scc", nullptr};
} // namespace

clap_plugin_descriptor SccVgmPlugin::desc = {
    CLAP_VERSION,
    SCC_VGM_PLUGIN_ID,
    SCC_VGM_PLUGIN_NAME,
    SCC_VGM_PLUGIN_VENDOR,
    SCC_VGM_PLUGIN_URL,
    "",
    "",
    SCC_VGM_PLUGIN_VERSION_STRING,
    SCC_VGM_PLUGIN_DESCRIPTION,
    features,
};

SccVgmPlugin::SccVgmPlugin(const clap_host* host)
    : clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Ignore,
                            clap::helpers::CheckingLevel::Minimal>(&desc, host),
      engine_(std::make_unique<VgmSccEngine>()) {}

SccVgmPlugin::~SccVgmPlugin() = default;

bool SccVgmPlugin::activate(double sampleRate, uint32_t /*minFrameCount*/,
                            uint32_t maxFrameCount) noexcept {
  engine_->prepare(sampleRate, maxFrameCount);
  return true;
}

void SccVgmPlugin::deactivate() noexcept {}

bool SccVgmPlugin::audioPortsInfo(uint32_t index, bool isInput,
                                  clap_audio_port_info* info) const noexcept {
  if (isInput || index > 9) {
    return false;
  }
  std::memset(info, 0, sizeof(*info));
  info->id = index;
  info->in_place_pair = CLAP_INVALID_ID;
  const char* name = "PSG CV Noise";
  if (index == 0) {
    std::strncpy(info->name, "Main", sizeof(info->name));
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    return true;
  }
  if (index == 1) {
    name = "CV Hz";
  } else if (index == 2) {
    name = "CV Vol";
  } else if (index == 3) {
    name = "Wave";
  } else if (index == 5) {
    name = "PSG Tone";
  } else if (index == 6) {
    name = "PSG Noise";
  } else if (index == 7) {
    name = "PSG Mixer";
  } else if (index == 8) {
    name = "PSG Env Shape";
  } else if (index == 9) {
    name = "PSG Env Period";
  }
  std::strncpy(info->name, name, sizeof(info->name));
  info->channel_count = 1;
  info->port_type = CLAP_PORT_MONO;
  return true;
}

bool SccVgmPlugin::notePortsInfo(uint32_t index, bool isInput,
                                 clap_note_port_info* info) const noexcept {
  if (isInput || index != 0) {
    return false;
  }
  info->id = 0;
  info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
  info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
  std::strncpy(info->name, "SCC Pitch", CLAP_NAME_SIZE);
  return true;
}

SccVgmPlugin::NoteOutStats SccVgmPlugin::noteOutStats() const {
  NoteOutStats s;
  s.noteOns = noteOnCount_.load(std::memory_order_relaxed);
  s.noteOffs = noteOffCount_.load(std::memory_order_relaxed);
  s.pushFails = notePushFailCount_.load(std::memory_order_relaxed);
  s.lastKey = static_cast<int16_t>(lastNoteKey_.load(std::memory_order_relaxed));
  return s;
}

uint32_t SccVgmPlugin::paramsCount() const noexcept { return 8; }

bool SccVgmPlugin::isValidParamId(clap_id paramId) const noexcept {
  return paramId >= pChannel && paramId <= pPitchBend;
}

bool SccVgmPlugin::paramsInfo(uint32_t paramIndex, clap_param_info* info) const noexcept {
  std::memset(info, 0, sizeof(*info));
  info->flags = CLAP_PARAM_IS_AUTOMATABLE;
  auto set = [&](clap_id id, const char* name, const char* module, double minV, double maxV,
                 double defV) {
    info->id = id;
    std::strncpy(info->name, name, CLAP_NAME_SIZE);
    std::strncpy(info->module, module, CLAP_PATH_SIZE);
    info->min_value = minV;
    info->max_value = maxV;
    info->default_value = defV;
  };
  switch (paramIndex) {
  case 0:
    set(pChannel, "Channel", "VGM", 1, 8, 1);
    info->flags |= CLAP_PARAM_IS_STEPPED;
    return true;
  case 1:
    set(pStartOffset, "Start Offset", "VGM", 0, 600, 0);
    return true;
  case 2:
    set(pNote, "Note", "Tuner", 0, 127, 0);
    info->flags |= CLAP_PARAM_IS_READONLY | CLAP_PARAM_IS_STEPPED;
    return true;
  case 3:
    set(pCents, "Cents", "Tuner", -100, 100, 0);
    info->flags |= CLAP_PARAM_IS_READONLY;
    return true;
  case 4:
    set(pHz, "Hz", "Tuner", 0, 20000, 0);
    info->flags |= CLAP_PARAM_IS_READONLY;
    return true;
  case 5:
    set(pGate, "Gate", "Tuner", 0, 1, 0);
    info->flags |= CLAP_PARAM_IS_READONLY | CLAP_PARAM_IS_STEPPED;
    return true;
  case 6:
    set(pVol, "Vol", "Tuner", 0, 1, 0);
    info->flags |= CLAP_PARAM_IS_READONLY;
    return true;
  case 7:
    set(pPitchBend, "MIDI Pitch Bend", "Notes", 0, 1, 0);
    info->flags |= CLAP_PARAM_IS_STEPPED;
    return true;
  default:
    return false;
  }
}

double SccVgmPlugin::getParam(clap_id id) const {
  const auto t = engine_->tuner();
  switch (id) {
  case pChannel: {
    const int ch = engine_->channel();
    return ch < 0 ? 1.0 : static_cast<double>(ch + 1);
  }
  case pStartOffset:
    return engine_->startOffsetSec();
  case pNote:
    return t.midiNote;
  case pCents:
    return t.cents;
  case pHz:
    return t.hz;
  case pGate:
    return (t.gate && t.active) ? 1.0 : 0.0;
  case pVol:
    return t.loudness();
  case pPitchBend:
    return pitchBendEnabled() ? 1.0 : 0.0;
  default:
    return 0;
  }
}

bool SccVgmPlugin::paramsValue(clap_id paramId, double* value) noexcept {
  if (!isValidParamId(paramId)) {
    return false;
  }
  *value = getParam(paramId);
  return true;
}

bool SccVgmPlugin::paramsValueToText(clap_id paramId, double value, char* display,
                                     uint32_t size) noexcept {
  if (paramId == pChannel) {
    const int n = static_cast<int>(value + 0.5);
    if (n >= 6 && n <= 8) {
      std::snprintf(display, size, "%c", 'A' + (n - 6));
    } else {
      std::snprintf(display, size, "%d", n);
    }
    return true;
  }
  if (paramId == pGate || paramId == pPitchBend) {
    std::snprintf(display, size, "%s", value >= 0.5 ? "On" : "Off");
    return true;
  }
  if (paramId == pCents || paramId == pHz || paramId == pStartOffset || paramId == pVol) {
    std::snprintf(display, size, "%.2f", value);
    return true;
  }
  std::snprintf(display, size, "%.0f", value);
  return true;
}

void SccVgmPlugin::applyParam(clap_id id, double value) {
  switch (id) {
  case pChannel: {
    int ch = static_cast<int>(value + 0.5) - 1;
    if (ch >= 8 && ch <= 10) {
      ch = 5 + (ch - 8);
    }
    ch = std::clamp(ch, 0, 7);
    engine_->setChannel(ch);
    break;
  }
  case pStartOffset:
    engine_->setStartOffsetSec(value);
    break;
  case pPitchBend:
    pitchBendEnabled_.store(value >= 0.5, std::memory_order_relaxed);
    break;
  default:
    break;
  }
}

void SccVgmPlugin::handleEvent(const clap_event_header_t* evt) {
  if (evt->space_id != CLAP_CORE_EVENT_SPACE_ID) {
    return;
  }
  if (evt->type == CLAP_EVENT_PARAM_VALUE) {
    auto* p = reinterpret_cast<const clap_event_param_value*>(evt);
    applyParam(p->param_id, p->value);
  }
}

void SccVgmPlugin::handleUiMessages() {
  UiMsg msg;
  while (fromUiQ.try_dequeue(msg)) {
    if (msg.type == UiMsg::LoadVgm) {
      std::string path = showOpenVgmDialog();
      if (path.empty()) {
        continue;
      }
      std::string err;
      engine_->loadPath(path, err);
    } else if (msg.type == UiMsg::SetChannel) {
      engine_->setChannel(msg.channel);
    } else if (msg.type == UiMsg::SetOffset) {
      engine_->setStartOffsetSec(msg.offset);
    } else if (msg.type == UiMsg::TestNote) {
      testNotePending_.store(true, std::memory_order_relaxed);
    } else if (msg.type == UiMsg::SetPitchBend) {
      pitchBendEnabled_.store(msg.enabled, std::memory_order_relaxed);
    }
  }
}

bool SccVgmPlugin::pushEvent(const clap_output_events_t* out, const clap_event_header_t* ev) {
  if (!out || !ev) {
    notePushFailCount_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!out->try_push(out, ev)) {
    notePushFailCount_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

void SccVgmPlugin::emitNotes(const clap_output_events_t* out, uint32_t time, uint32_t frames) {
  if (!out) {
    notePushFailCount_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t offTime = frames > 1 ? frames - 1 : time;
  const bool bendOn = pitchBendEnabled();
  // MIDI pitch bend assumes ±2 semitones at the receiving synth.
  constexpr float kBendRangeCents = 200.f;

  auto pushMidi = [&](uint8_t status, uint8_t d1, uint8_t d2, uint32_t t) {
    clap_event_midi m{};
    m.header.size = sizeof(m);
    m.header.time = t;
    m.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    m.header.type = CLAP_EVENT_MIDI;
    m.header.flags = 0;
    m.port_index = 0;
    m.data[0] = status;
    m.data[1] = d1;
    m.data[2] = d2;
    pushEvent(out, &m.header);
  };

  auto pushPitchBend = [&](float tuningCents, uint32_t t) {
    float cents = bendOn ? tuningCents : 0.f;
    cents = std::clamp(cents, -kBendRangeCents, kBendRangeCents);
    const int bend =
        8192 + static_cast<int>(std::lround((cents / kBendRangeCents) * 8192.0));
    const int clamped = std::clamp(bend, 0, 16383);
    pushMidi(0xE0, static_cast<uint8_t>(clamped & 0x7f), static_cast<uint8_t>((clamped >> 7) & 0x7f),
             t);
  };

  auto pushClapTuning = [&](int32_t noteId, int16_t key, float tuningCents, uint32_t t) {
    // CLAP tuning is relative semitones (value * 100 = cents).
    clap_event_note_expression ex{};
    ex.header.size = sizeof(ex);
    ex.header.time = t;
    ex.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ex.header.type = CLAP_EVENT_NOTE_EXPRESSION;
    ex.expression_id = CLAP_NOTE_EXPRESSION_TUNING;
    ex.note_id = noteId;
    ex.port_index = 0;
    ex.channel = 0;
    ex.key = key;
    ex.value = static_cast<double>(tuningCents) / 100.0;
    pushEvent(out, &ex.header);
  };

  auto pushNoteOff = [&](int32_t noteId, int16_t key, uint32_t t) {
    clap_event_note n{};
    n.header.size = sizeof(n);
    n.header.time = t;
    n.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    n.header.type = CLAP_EVENT_NOTE_OFF;
    n.header.flags = 0;
    n.note_id = noteId;
    n.port_index = 0;
    n.channel = 0;
    n.key = key;
    n.velocity = 0;
    if (pushEvent(out, &n.header)) {
      noteOffCount_.fetch_add(1, std::memory_order_relaxed);
    }
    pushMidi(0x80, static_cast<uint8_t>(std::clamp<int>(key, 0, 127)), 0, t);
    pushPitchBend(0.f, t);
  };

  auto pushNoteOn = [&](int32_t noteId, int16_t key, float tuningCents, uint32_t t) {
    clap_event_note n{};
    n.header.size = sizeof(n);
    n.header.time = t;
    n.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    n.header.type = CLAP_EVENT_NOTE_ON;
    n.header.flags = 0;
    n.note_id = noteId;
    n.port_index = 0;
    n.channel = 0;
    n.key = key;
    n.velocity = 1.0;
    if (pushEvent(out, &n.header)) {
      noteOnCount_.fetch_add(1, std::memory_order_relaxed);
      lastNoteKey_.store(key, std::memory_order_relaxed);
    }
    pushPitchBend(tuningCents, t);
    pushMidi(0x90, static_cast<uint8_t>(std::clamp<int>(key, 0, 127)), 127, t);
    pushClapTuning(noteId, key, tuningCents, t);
  };

  if (testNotePending_.exchange(false, std::memory_order_relaxed)) {
    if (activeNoteId_ >= 0) {
      pushNoteOff(activeNoteId_, activeKey_, time);
      activeNoteId_ = -1;
      activeKey_ = -1;
    }
    activeNoteId_ = noteIdCounter_++;
    activeKey_ = 60;
    pushNoteOn(activeNoteId_, activeKey_, 0.f, time);
    pushNoteOff(activeNoteId_, activeKey_, offTime);
    activeNoteId_ = -1;
    activeKey_ = -1;
  }

  std::vector<VgmSccEngine::NoteEvent> evs;
  engine_->drainNoteEvents(evs);
  for (const auto& e : evs) {
    if (e.type == VgmSccEngine::NoteEvent::Off) {
      if (activeNoteId_ < 0) {
        continue;
      }
      const int16_t key = activeKey_ >= 0 ? activeKey_ : static_cast<int16_t>(e.key);
      pushNoteOff(activeNoteId_, key, time);
      activeNoteId_ = -1;
      activeKey_ = -1;
    } else if (e.type == VgmSccEngine::NoteEvent::On) {
      if (activeNoteId_ >= 0) {
        pushNoteOff(activeNoteId_, activeKey_, time);
      }
      activeNoteId_ = noteIdCounter_++;
      activeKey_ = static_cast<int16_t>(e.key);
      pushNoteOn(activeNoteId_, activeKey_, static_cast<float>(e.tuningCents), time);
    } else if (e.type == VgmSccEngine::NoteEvent::Tuning && activeNoteId_ >= 0) {
      const float cents = static_cast<float>(e.tuningCents);
      pushClapTuning(activeNoteId_, activeKey_, cents, time);
      if (bendOn) {
        pushPitchBend(cents, time);
      }
    }
  }
}

void SccVgmPlugin::emitTunerParams(const clap_output_events_t* out, uint32_t time) {
  if (!out) {
    return;
  }
  const auto t = engine_->tuner();
  const double gate = (t.gate && t.active) ? 1.0 : 0.0;
  const double note = (t.gate && t.active) ? static_cast<double>(t.midiNote) : 0.0;
  auto pushParam = [&](clap_id id, double value) {
    clap_event_param_value p{};
    p.header.size = sizeof(p);
    p.header.time = time;
    p.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    p.header.type = CLAP_EVENT_PARAM_VALUE;
    p.header.flags = 0;
    p.param_id = id;
    p.cookie = nullptr;
    p.note_id = -1;
    p.port_index = -1;
    p.channel = -1;
    p.key = -1;
    p.value = value;
    out->try_push(out, &p.header);
  };
  if (gate != lastPushedGate_) {
    lastPushedGate_ = gate;
    pushParam(pGate, gate);
  }
  if (note != lastPushedNote_) {
    lastPushedNote_ = note;
    pushParam(pNote, note);
  }
}

bool SccVgmPlugin::stateSave(const clap_ostream* stream) noexcept {
  std::vector<uint8_t> blob;
  if (!engine_->saveState(blob)) {
    return false;
  }
  int64_t written = 0;
  while (written < static_cast<int64_t>(blob.size())) {
    auto n = stream->write(stream, blob.data() + written, blob.size() - static_cast<size_t>(written));
    if (n <= 0) {
      return false;
    }
    written += n;
  }
  return true;
}

bool SccVgmPlugin::stateLoad(const clap_istream* stream) noexcept {
  std::vector<uint8_t> data;
  uint8_t buf[4096];
  while (true) {
    auto n = stream->read(stream, buf, sizeof(buf));
    if (n < 0) {
      return false;
    }
    if (n == 0) {
      break;
    }
    data.insert(data.end(), buf, buf + n);
  }
  std::string err;
  return engine_->loadState(data.data(), data.size(), err);
}

clap_process_status SccVgmPlugin::process(const clap_process* process) noexcept {
  handleUiMessages();

  auto* inEv = process->in_events;
  const uint32_t sz = inEv ? inEv->size(inEv) : 0;
  for (uint32_t i = 0; i < sz; ++i) {
    handleEvent(inEv->get(inEv, i));
  }

  if (process->audio_outputs_count <= 0 || !process->audio_outputs[0].data32) {
    emitNotes(process->out_events, 0, process->frames_count);
    emitTunerParams(process->out_events, 0);
    return CLAP_PROCESS_SLEEP;
  }

  float* left = process->audio_outputs[0].data32[0];
  float* right = process->audio_outputs[0].channel_count > 1 ? process->audio_outputs[0].data32[1]
                                                             : left;
  float* cvHz = nullptr;
  float* cvVol = nullptr;
  float* wave = nullptr;
  float* cvNoise = nullptr;
  float* tone = nullptr;
  float* noise = nullptr;
  float* cvMixer = nullptr;
  float* cvEnvShape = nullptr;
  float* cvEnvPeriod = nullptr;
  if (process->audio_outputs_count > 1 && process->audio_outputs[1].data32 &&
      process->audio_outputs[1].channel_count >= 1) {
    cvHz = process->audio_outputs[1].data32[0];
  }
  if (process->audio_outputs_count > 2 && process->audio_outputs[2].data32 &&
      process->audio_outputs[2].channel_count >= 1) {
    cvVol = process->audio_outputs[2].data32[0];
  }
  if (process->audio_outputs_count > 3 && process->audio_outputs[3].data32 &&
      process->audio_outputs[3].channel_count >= 1) {
    wave = process->audio_outputs[3].data32[0];
  }
  if (process->audio_outputs_count > 4 && process->audio_outputs[4].data32 &&
      process->audio_outputs[4].channel_count >= 1) {
    cvNoise = process->audio_outputs[4].data32[0];
  }
  if (process->audio_outputs_count > 5 && process->audio_outputs[5].data32 &&
      process->audio_outputs[5].channel_count >= 1) {
    tone = process->audio_outputs[5].data32[0];
  }
  if (process->audio_outputs_count > 6 && process->audio_outputs[6].data32 &&
      process->audio_outputs[6].channel_count >= 1) {
    noise = process->audio_outputs[6].data32[0];
  }
  if (process->audio_outputs_count > 7 && process->audio_outputs[7].data32 &&
      process->audio_outputs[7].channel_count >= 1) {
    cvMixer = process->audio_outputs[7].data32[0];
  }
  if (process->audio_outputs_count > 8 && process->audio_outputs[8].data32 &&
      process->audio_outputs[8].channel_count >= 1) {
    cvEnvShape = process->audio_outputs[8].data32[0];
  }
  if (process->audio_outputs_count > 9 && process->audio_outputs[9].data32 &&
      process->audio_outputs[9].channel_count >= 1) {
    cvEnvPeriod = process->audio_outputs[9].data32[0];
  }

  bool playing = false;
  double songSec = 0.0;
  if (process->transport) {
    const auto* t = process->transport;
    playing = (t->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
    if (t->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE) {
      songSec = static_cast<double>(t->song_pos_seconds) / static_cast<double>(CLAP_SECTIME_FACTOR);
    }
  }

  engine_->process(left, right, cvHz, cvVol, wave, cvNoise, tone, noise, cvMixer, cvEnvShape,
                   cvEnvPeriod, process->frames_count, songSec, playing);
  emitNotes(process->out_events, 0, process->frames_count);
  emitTunerParams(process->out_events, 0);
  return CLAP_PROCESS_CONTINUE;
}

void SccVgmPlugin::paramsFlush(const clap_input_events* in,
                               const clap_output_events* /*out*/) noexcept {
  handleUiMessages();
  if (!in) {
    return;
  }
  const uint32_t sz = in->size(in);
  for (uint32_t i = 0; i < sz; ++i) {
    handleEvent(in->get(in, i));
  }
}

bool SccVgmPlugin::guiIsApiSupported(const char* api, bool isFloating) noexcept {
  if (isFloating) {
    return false;
  }
  return std::strcmp(api, CLAP_WINDOW_API_WIN32) == 0;
}

bool SccVgmPlugin::guiCreate(const char* /*api*/, bool /*isFloating*/) noexcept {
  editor_ = new SccVgmEditor(this);
  const clap_host_timer_support_t* timer{nullptr};
  _host.getExtension(timer, CLAP_EXT_TIMER_SUPPORT);
  return imgui_clap_guiCreateWith(editor_, timer);
}

void SccVgmPlugin::guiDestroy() noexcept {
  if (!editor_) {
    return;
  }
  const clap_host_timer_support_t* timer{nullptr};
  _host.getExtension(timer, CLAP_EXT_TIMER_SUPPORT);
  if (editor_->ctx) {
    imgui_clap_guiDestroyWith(editor_, timer);
  } else {
    editor_->onGuiDestroy();
  }
  delete editor_;
  editor_ = nullptr;
}

bool SccVgmPlugin::guiSetParent(const clap_window* window) noexcept {
  return imgui_clap_guiSetParentWith(editor_, window);
}

bool SccVgmPlugin::guiSetScale(double /*scale*/) noexcept { return false; }

bool SccVgmPlugin::guiSetSize(uint32_t width, uint32_t height) noexcept {
  guiW_ = width;
  guiH_ = height;
  return imgui_clap_guiSetSizeWith(editor_, static_cast<int>(width), static_cast<int>(height));
}

bool SccVgmPlugin::guiGetSize(uint32_t* width, uint32_t* height) noexcept {
  *width = guiW_;
  *height = guiH_;
  return true;
}

bool SccVgmPlugin::guiAdjustSize(uint32_t* /*width*/, uint32_t* /*height*/) noexcept { return true; }

bool SccVgmPlugin::guiShow() noexcept { return editor_ && editor_->ctx; }

bool SccVgmPlugin::guiHide() noexcept { return true; }

} // namespace scc
