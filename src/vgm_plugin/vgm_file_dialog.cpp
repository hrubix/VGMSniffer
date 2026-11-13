#include "vgm_file_dialog.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>

namespace scc {

std::string showOpenVgmDialog(void* parentHwnd) {
  char file[MAX_PATH]{};
  OPENFILENAMEA ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = static_cast<HWND>(parentHwnd);
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = "VGM / VGZ\0*.vgm;*.vgz\0All\0*.*\0";
  ofn.nFilterIndex = 1;
  ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameA(&ofn)) {
    return {};
  }
  return std::string(file);
}

} // namespace scc
