#include "scc_vgm_plugin.h"

#include <cstring>

namespace scc::vgmpluginentry {

uint32_t clap_get_plugin_count(const clap_plugin_factory*) { return 1; }

const clap_plugin_descriptor* clap_get_plugin_descriptor(const clap_plugin_factory*, uint32_t) {
  return &SccVgmPlugin::desc;
}

const clap_plugin* clap_create_plugin(const clap_plugin_factory*, const clap_host* host,
                                      const char* plugin_id) {
  if (std::strcmp(plugin_id, SccVgmPlugin::desc.id) != 0) {
    return nullptr;
  }
  auto* p = new SccVgmPlugin(host);
  return p->clapPlugin();
}

const CLAP_EXPORT clap_plugin_factory scc_vgm_factory = {
    clap_get_plugin_count,
    clap_get_plugin_descriptor,
    clap_create_plugin,
};

const void* get_factory(const char* factory_id) {
  return std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &scc_vgm_factory : nullptr;
}

bool clap_init(const char*) { return true; }
void clap_deinit() {}

} // namespace scc::vgmpluginentry

extern "C" {
const CLAP_EXPORT clap_plugin_entry clap_entry = {
    CLAP_VERSION,
    scc::vgmpluginentry::clap_init,
    scc::vgmpluginentry::clap_deinit,
    scc::vgmpluginentry::get_factory,
};
}
