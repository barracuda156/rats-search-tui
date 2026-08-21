#pragma once

#include <filesystem>
#include <string>

namespace ratsn::platform {

// ~/.rats-native (or the platform equivalent of $HOME); does not touch disk.
std::filesystem::path defaultDataDir();

// `override_dir` wins when non-empty, else defaultDataDir(). Creates the
// directory (and its "index" subdirectory) if missing.
std::filesystem::path resolveDataDir(const std::string& override_dir = {});

std::filesystem::path indexDir(const std::filesystem::path& data_dir);
std::filesystem::path configFile(const std::filesystem::path& data_dir);
std::filesystem::path downloadsFile(const std::filesystem::path& data_dir);

} // namespace ratsn::platform
