#include "scc_vgm_editor.h"

#include "scc_vgm_version.h"

#include "imgui.h"

#include <cstdio>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>

namespace scc {

namespace {

ImVec4 rgb(int r, int g, int b) {
  return ImVec4(r / 255.f, g / 255.f, b / 255.f, 1.f);
}

// THEME_DEFAULT.css and THEME_DEFAULT.channel from VGM-editor web/theme.js.
const ImVec4 kBg = rgb(15, 20, 25);
const ImVec4 kPanel = rgb(26, 35, 50);
const ImVec4 kBorder = rgb(45, 58, 79);
const ImVec4 kText = rgb(231, 236, 243);
const ImVec4 kMuted = rgb(139, 156, 179);
const ImVec4 kAccent = rgb(61, 139, 253);
/** Selected channel / control — darker blue than accent hover. */
const ImVec4 kSelected = rgb(28, 72, 148);
const ImVec4 kButton = rgb(36, 48, 68);

const ImVec4 kSccLabel[5] = {rgb(192, 112, 184), rgb(168, 104, 192), rgb(136, 104, 196),
                             rgb(104, 104, 200), rgb(80, 112, 208)};
const ImVec4 kPsgLabel[3] = {rgb(70, 150, 224), rgb(75, 196, 138), rgb(217, 148, 72)};

void applyTheme() {
  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowRounding = 6.f;
  st.FrameRounding = 4.f;
  st.GrabRounding = 3.f;
  st.Colors[ImGuiCol_WindowBg] = kBg;
  st.Colors[ImGuiCol_ChildBg] = kPanel;
  st.Colors[ImGuiCol_Text] = kText;
  st.Colors[ImGuiCol_TextDisabled] = kMuted;
  st.Colors[ImGuiCol_Button] = kButton;
  st.Colors[ImGuiCol_ButtonHovered] = kAccent;
  st.Colors[ImGuiCol_ButtonActive] = kAccent;
  st.Colors[ImGuiCol_FrameBg] = rgb(18, 26, 36);
  st.Colors[ImGuiCol_Header] = kButton;
  st.Colors[ImGuiCol_Border] = kBorder;
}

bool channelButton(const char* label, bool selected, ImVec4 textColor, int channel, SccVgmPlugin* plugin) {
  ImGui::PushStyleColor(ImGuiCol_Text, textColor);
  if (selected) {
    ImGui::PushStyleColor(ImGuiCol_Button, kSelected);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kSelected);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kSelected);
  }
  const bool pressed = ImGui::Button(label, ImVec2(48, 32));
  if (selected) {
    ImGui::PopStyleColor(3);
  }
  ImGui::PopStyleColor();
  if (pressed) {
    SccVgmPlugin::UiMsg msg;
    msg.type = SccVgmPlugin::UiMsg::SetChannel;
    msg.channel = channel;
    plugin->fromUiQ.enqueue(msg);
  }
  return pressed;
}

bool decodePngRgba(const uint8_t* data, DWORD size, std::vector<uint8_t>& rgba, int& width, int& height) {
  const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool uninit = co == S_OK || co == S_FALSE;
  IWICImagingFactory* factory = nullptr;
  IWICStream* stream = nullptr;
  IWICBitmapDecoder* decoder = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* conv = nullptr;
  bool ok = false;
  if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
      SUCCEEDED(factory->CreateStream(&stream)) &&
      SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(data), size)) &&
      SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) &&
      SUCCEEDED(decoder->GetFrame(0, &frame))) {
    UINT w = 0;
    UINT h = 0;
    if (SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0 &&
        SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.f,
                                   WICBitmapPaletteTypeCustom))) {
      rgba.resize(static_cast<size_t>(w) * h * 4u);
      if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4u, static_cast<UINT>(rgba.size()), rgba.data()))) {
        width = static_cast<int>(w);
        height = static_cast<int>(h);
        ok = true;
      }
    }
  }
  if (conv) {
    conv->Release();
  }
  if (frame) {
    frame->Release();
  }
  if (decoder) {
    decoder->Release();
  }
  if (stream) {
    stream->Release();
  }
  if (factory) {
    factory->Release();
  }
  if (uninit) {
    CoUninitialize();
  }
  return ok;
}

bool loadRcPng(const wchar_t* name, std::vector<uint8_t>& bytes) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&loadRcPng), &module)) {
    return false;
  }
  HRSRC res = FindResourceW(module, name, MAKEINTRESOURCEW(10));
  if (!res) {
    return false;
  }
  HGLOBAL glob = LoadResource(module, res);
  if (!glob) {
    return false;
  }
  const auto* data = static_cast<const uint8_t*>(LockResource(glob));
  const DWORD size = SizeofResource(module, res);
  if (!data || size == 0) {
    return false;
  }
  bytes.assign(data, data + size);
  return true;
}

bool ensureTexture(SccVgmEditor* editor, bool& tried, void*& tex, int& tw, int& th, const wchar_t* rcName) {
  if (tried) {
    return tex != nullptr && tw > 0 && th > 0;
  }
  tried = true;
  std::vector<uint8_t> png;
  std::vector<uint8_t> rgba;
  int w = 0;
  int h = 0;
  if (loadRcPng(rcName, png) && decodePngRgba(png.data(), static_cast<DWORD>(png.size()), rgba, w, h)) {
    void* t = nullptr;
    if (imgui_clap_create_texture_rgba(editor, rgba.data(), w, h, &t) && t) {
      tex = t;
      tw = w;
      th = h;
    }
  }
  return tex != nullptr && tw > 0 && th > 0;
}

void drawMascot(SccVgmEditor* editor) {
  if (!ensureTexture(editor, editor->mascotTried, editor->mascotTex, editor->mascotW, editor->mascotH,
                     L"SNIFFER")) {
    return;
  }
  const float colW = ImGui::GetContentRegionAvail().x * 0.5f;
  const float drawH = colW * (static_cast<float>(editor->mascotH) / static_cast<float>(editor->mascotW));
  if (ImGui::BeginTable("mascot", 2, ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableNextColumn();
    ImGui::Image(static_cast<ImTextureID>(editor->mascotTex), ImVec2(colW, drawH));
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s - \xC2\xA9 2026 Rubikon Lab (FKA FAC) - alpha version / contact & "
                       "bugreports: rubikonlab@proton.me | emu2149 & emu2212 \xC2\xA9 Mitsutaka Okazaki",
                       SCC_VGM_PLUGIN_VERSION_STRING);
    ImGui::EndTable();
  }
}

void drawHeader(SccVgmEditor* editor) {
  const float logoH = 36.f;
  if (ensureTexture(editor, editor->logoTried, editor->logoTex, editor->logoW, editor->logoH, L"RUBIKON")) {
    const float logoW =
        logoH * (static_cast<float>(editor->logoW) / static_cast<float>(editor->logoH));
    ImGui::Image(static_cast<ImTextureID>(editor->logoTex), ImVec2(logoW, logoH));
    ImGui::SameLine();
    ImGui::BeginGroup();
  }
  ImGui::Text("%s  v%s", SCC_VGM_PLUGIN_NAME, SCC_VGM_PLUGIN_VERSION_STRING);
  ImGui::TextDisabled("by Rubikon Lab");
  ImGui::TextDisabled("%s", SCC_VGM_PLUGIN_DESCRIPTION);
  if (editor->logoTex) {
    ImGui::EndGroup();
  }
}

} // namespace

SccVgmEditor::SccVgmEditor(SccVgmPlugin* p) : plugin(p) {}

void SccVgmEditor::onGuiCreate() {}

void SccVgmEditor::onRender() {
  if (!plugin) {
    return;
  }
  applyTheme();

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
  ImGui::Begin("##sccvgm", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoCollapse);

  drawHeader(this);
  ImGui::Separator();

  auto& eng = plugin->engine();
  const bool hasFile = eng.hasFile();
  const int ch = eng.channel();
  const bool armed = eng.isArmed();

  if (ImGui::Button("Load VGM")) {
    SccVgmPlugin::UiMsg msg;
    msg.type = SccVgmPlugin::UiMsg::LoadVgm;
    plugin->fromUiQ.enqueue(msg);
  }

  if (!hasFile) {
    ImGui::Spacing();
    ImGui::TextDisabled("No VGM loaded — choose a file, then pick a channel.");
    ImGui::Spacing();
    drawMascot(this);
    ImGui::End();
    return;
  }

  ImGui::SameLine();
  ImGui::TextWrapped("%s  (%.1f s%s)", eng.fileName().c_str(), eng.durationSec(),
                     eng.loops() ? ", loops" : "");

  ImGui::Spacing();
  if (eng.hasPsg()) {
    ImGui::AlignTextToFramePadding();
    ImGui::Text("PSG");
    for (int i = 0; i < 3; ++i) {
      ImGui::SameLine();
      char lab[4] = {static_cast<char>('A' + i), 0, 0, 0};
      channelButton(lab, ch == 5 + i, kPsgLabel[i], 5 + i, plugin);
    }
  }
  if (eng.hasScc()) {
    ImGui::AlignTextToFramePadding();
    ImGui::Text("SCC");
    for (int i = 0; i < 5; ++i) {
      ImGui::SameLine();
      char lab[8];
      std::snprintf(lab, sizeof(lab), "%d", i + 1);
      channelButton(lab, ch == i, kSccLabel[i], i, plugin);
    }
  }

  if (ch < 0) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.91f, 0.58f, 0.28f, 1.f), "Select a channel to process");
    ImGui::Spacing();
    drawMascot(this);
    ImGui::End();
    return;
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::BeginChild("tuner", ImVec2(0, 320), true);
  const auto t = eng.tuner();
  const bool noteGate = t.gate && t.active;
  ImGui::Text("Status: %s", noteGate ? "ON" : "off");
  ImGui::Text("Note:   %s", t.noteName);
  ImGui::Text("MIDI:   %d", t.midiNote);
  ImGui::Text("Hz:     %.2f", t.hz);
  ImGui::Text("Cents:  %+.1f", t.cents);
  ImGui::Text("Period: %u", static_cast<unsigned>(t.period));
  if (t.envelope) {
    ImGui::Text("Volume: envelope");
  } else {
    ImGui::Text("Volume: %u (0=silent, 15=loud)", static_cast<unsigned>(t.volume));
  }
  ImGui::Text("Vol:    %.2f", t.loudness());
  ImGui::Text("CV Hz:  %.4f (= Hz/%.0f)", t.cvHz(), static_cast<double>(kCvHzScale));
  if (ch >= 5) {
    ImGui::Text("Noise:  %.1f Hz", t.noiseHz);
  }
  if (eng.hasPsg()) {
    ImGui::Separator();
    ImGui::Text("PSG Mixer:      0x%02X  (CV %.4f)", static_cast<unsigned>(t.psgMixerRaw),
                t.psgMixer);
    ImGui::Text("PSG Env Shape:  %u     (CV %.4f)", static_cast<unsigned>(t.psgEnvShapeRaw),
                t.psgEnvShape);
    ImGui::Text("PSG Env Period: %u  (CV %.4f)", static_cast<unsigned>(t.psgEnvPeriodRaw),
                t.psgEnvPeriod);
  }
  ImGui::Separator();
  bool bend = plugin->pitchBendEnabled();
  if (ImGui::Checkbox("MIDI pitch bend (±2 semitones)", &bend)) {
    SccVgmPlugin::UiMsg msg;
    msg.type = SccVgmPlugin::UiMsg::SetPitchBend;
    msg.enabled = bend;
    plugin->fromUiQ.enqueue(msg);
  }
  ImGui::TextDisabled("Set the synth pitch-bend range to ±2 st.");
  const auto ns = plugin->noteOutStats();
  ImGui::Text("Notes out: %u on / %u off", ns.noteOns, ns.noteOffs);
  if (ns.lastKey >= 0) {
    ImGui::Text("Last key:  %d", static_cast<int>(ns.lastKey));
  } else {
    ImGui::Text("Last key:  —");
  }
  if (ns.pushFails > 0) {
    ImGui::TextColored(ImVec4(0.91f, 0.45f, 0.35f, 1.f), "Push fails: %u", ns.pushFails);
  }
  if (ImGui::Button("Send test note (C4)")) {
    SccVgmPlugin::UiMsg msg;
    msg.type = SccVgmPlugin::UiMsg::TestNote;
    plugin->fromUiQ.enqueue(msg);
  }
  ImGui::EndChild();

  if (!armed) {
    ImGui::TextDisabled("Not armed");
  }

  ImGui::Spacing();
  drawMascot(this);

  ImGui::End();
}

} // namespace scc
