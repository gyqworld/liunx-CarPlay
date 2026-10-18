#pragma once

#include "carplay/platform/storage.hpp"
#include "carplay/session/runtime_config.hpp"

namespace carplay {

bool save_runtime_config(const IStorage& storage, const RuntimeConfig& config);
bool load_runtime_config(const IStorage& storage, RuntimeConfig& config);

}  // namespace carplay
