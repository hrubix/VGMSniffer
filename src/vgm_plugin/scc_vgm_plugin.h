#pragma once

#include "vgm_scc_engine.h"

#include <clap/helpers/plugin.hh>
#include <readerwriterqueue.h>

#include <atomic>
#include <memory>

namespace scc {

struct SccVgmEditor;

struct SccVgmPlugin : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Ignore,
                                                   clap::helpers::CheckingLevel::Minimal> {
  static clap_plugin_descriptor desc;

  explicit SccVgmPlugin(const clap_host* host);
  ~SccVgmPlugin() override;

  bool activate(double sampleRate, uint32_t minFrameCount, uint32_t maxFrameCount) noexcept override;
  void deactivate() noexcept override;

  bool implementsAudioPorts() const noexcept override { return true; }
  uint32_t audioPortsCount(bool isInput) const noexcept override { return isInput ? 0 : 10; }
  bool audioPortsInfo(uint32_t index, bool isInput, clap_audio_port_info* info) const noexcept override;

  bool implementsNotePorts() const noexcept override { return true; }
  uint32_t notePortsCount(bool isInput) const noexcept override { return isInput ? 0 : 1; }
  bool notePortsInfo(uint32_t index, bool isInput, clap_note_port_info* info) const noexcept override;

  bool implementsParams() const noexcept override { return true; }
  uint32_t paramsCount() const noexcept override;
  bool paramsInfo(uint32_t paramIndex, clap_param_info* info) const noexcept override;
  bool paramsValue(clap_id paramId, double* value) noexcept override;
  bool paramsValueToText(clap_id paramId, double value, char* display, uint32_t size) noexcept override;
  bool isValidParamId(clap_id paramId) const noexcept override;

  bool implementsState() const noexcept override { return true; }
  bool stateSave(const clap_ostream* stream) noexcept override;
  bool stateLoad(const clap_istream* stream) noexcept override;

  clap_process_status process(const clap_process* process) noexcept override;
  void paramsFlush(const clap_input_events* in, const clap_output_events* out) noexcept override;

  bool implementsGui() const noexcept override { return true; }
  bool guiIsApiSupported(const char* api, bool isFloating) noexcept override;
  bool guiCreate(const char* api, bool isFloating) noexcept override;
  void guiDestroy() noexcept override;
  bool guiSetParent(const clap_window* window) noexcept override;
  bool guiSetScale(double scale) noexcept override;
  bool guiCanResize() const noexcept override { return true; }
  bool guiAdjustSize(uint32_t* width, uint32_t* height) noexcept override;
  bool guiSetSize(uint32_t width, uint32_t height) noexcept override;
  bool guiGetSize(uint32_t* width, uint32_t* height) noexcept override;
  bool guiShow() noexcept override;
  bool guiHide() noexcept override;

  enum ParamId : clap_id {
    pChannel = 2001,
    pStartOffset = 2002,
    pNote = 2003,
    pCents = 2004,
    pHz = 2005,
    pGate = 2006,
    pVol = 2007, // chip gain vol/16 (0=mute, 15 → 15/16)
    pPitchBend = 2008, // MIDI pitch bend microtuning (±2 semitones)
  };

  /** When true, MIDI note outs include pitch bend for cents. CLAP tuning is always cents/100. */
  bool pitchBendEnabled() const {
    return pitchBendEnabled_.load(std::memory_order_relaxed);
  }

  static constexpr uint32_t kGuiW = 520;
  static constexpr uint32_t kGuiH = 940;

  VgmSccEngine& engine() { return *engine_; }

  struct UiMsg {
    enum Type { LoadVgm, SetChannel, SetOffset, TestNote, SetPitchBend } type = LoadVgm;
    int channel = -1;
    double offset = 0;
    bool enabled = false;
  };
  using UiQueue = moodycamel::ReaderWriterQueue<UiMsg>;
  UiQueue fromUiQ{64};

  void handleUiMessages();

  struct NoteOutStats {
    uint32_t noteOns = 0;
    uint32_t noteOffs = 0;
    uint32_t pushFails = 0;
    int16_t lastKey = -1;
  };
  NoteOutStats noteOutStats() const;

private:
  void handleEvent(const clap_event_header_t* evt);
  void applyParam(clap_id id, double value);
  double getParam(clap_id id) const;
  void emitNotes(const clap_output_events_t* out, uint32_t time, uint32_t frames);
  void emitTunerParams(const clap_output_events_t* out, uint32_t time);
  bool pushEvent(const clap_output_events_t* out, const clap_event_header_t* ev);

  std::unique_ptr<VgmSccEngine> engine_;
  SccVgmEditor* editor_ = nullptr;
  uint32_t guiW_ = kGuiW;
  uint32_t guiH_ = kGuiH;
  int32_t noteIdCounter_ = 1;
  int32_t activeNoteId_ = -1;
  int16_t activeKey_ = -1;
  double lastPushedNote_ = -1.0;
  double lastPushedGate_ = -1.0;
  std::atomic<uint32_t> noteOnCount_{0};
  std::atomic<uint32_t> noteOffCount_{0};
  std::atomic<uint32_t> notePushFailCount_{0};
  std::atomic<int32_t> lastNoteKey_{-1};
  std::atomic<bool> testNotePending_{false};
  std::atomic<bool> pitchBendEnabled_{false};
};

} // namespace scc
