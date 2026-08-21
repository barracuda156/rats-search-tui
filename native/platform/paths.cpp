#include "platform/paths.h"

#include <cstdlib>
#include <stdexcept>

namespace ratsn::platform {

std::filesystem::path defaultDataDir()
{
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
        home = std::getenv("USERPROFILE"); // Windows fallback
    }
    if (!home || !*home) {
        throw std::runtime_error("cannot resolve data directory: $HOME is not set");
    }
    return std::filesystem::path(home) / ".rats-native";
}

std::filesystem::path resolveDataDir(const std::string& override_dir)
{
    std::filesystem::path dir = override_dir.empty() ? defaultDataDir() : std::filesystem::path(override_dir);
    std::filesystem::create_directories(indexDir(dir));
    return dir;
}

std::filesystem::path indexDir(const std::filesystem::path& data_dir)
{
    return data_dir / "index";
}

std::filesystem::path configFile(const std::filesystem::path& data_dir)
{
    return data_dir / "rats-native.json";
}

std::filesystem::path downloadsFile(const std::filesystem::path& data_dir)
{
    return data_dir / "downloads.json";
}

} // namespace ratsn::platform
