#include "platform/log.h"

#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>

namespace ratsn::platform {

namespace {
std::mutex g_mutex;
std::unique_ptr<std::ofstream> g_file; // guarded by g_mutex; null = stdout
} // namespace

namespace detail {

LogLine::~LogLine()
{
    const std::string line = buf_.str();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        (*g_file) << line;
        g_file->flush();
    } else {
        std::cout << line;
    }
}

} // namespace detail

bool enableFileLogging(const std::string& path)
{
    auto file = std::make_unique<std::ofstream>(path, std::ios::out | std::ios::trunc);
    if (!file->is_open())
        return false;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_file = std::move(file);
    return true;
}

void disableFileLogging()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_file.reset();
}

} // namespace ratsn::platform
