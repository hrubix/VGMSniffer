#pragma once

#include "scc_vgm_plugin.h"

#include <imgui-clap-support/imgui-clap-editor.h>

namespace scc {

struct SccVgmEditor : public imgui_clap_editor {
  explicit SccVgmEditor(SccVgmPlugin* plugin);

  void onRender() override;
  void onGuiCreate() override;

  SccVgmPlugin* plugin = nullptr;
  void* mascotTex = nullptr;
  int mascotW = 0;
  int mascotH = 0;
  bool mascotTried = false;
  void* logoTex = nullptr;
  int logoW = 0;
  int logoH = 0;
  bool logoTried = false;
};

} // namespace scc
