#pragma once

#include <filesystem>

namespace container_ban {

/// @brief Load rules from <configDir>/config.json (create a default file if missing),
///        then install all hooks. @return True if every hook was installed.
bool enableContainerBan(std::filesystem::path const& configDir);

/// @brief Remove all installed hooks and clear the loaded rules.
void disableContainerBan();

} // namespace container_ban
