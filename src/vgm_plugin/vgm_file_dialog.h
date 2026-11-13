#pragma once

#include <string>

namespace scc {

/** Win32 open dialog for .vgm / .vgz. Empty on cancel. */
std::string showOpenVgmDialog(void* parentHwnd = nullptr);

} // namespace scc
