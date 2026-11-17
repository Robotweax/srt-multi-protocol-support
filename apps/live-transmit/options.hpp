#pragma once
#include <srtmux/transport.hpp>
#include "haivision/uriparser.hpp"
#include <map>

namespace live_transmit {
struct Options {
    bool help = false, version = false, quiet = false, verbose = false, reconnect = true;
    int timeout_seconds = 0, timeout_mode = 0, eof_grace_ms = 1000;
    size_t chunk = 1456, buffering = 10, stats_every = 0, bandwidth_every = 0;
    std::string provider = "haivision", source, target, stats_path, stats_format = "default";
    std::filesystem::path provider_dir;
    std::map<std::string, std::filesystem::path> modules;
};
struct EndpointOptions {
    enum class Kind { file, udp, srt } kind;
    std::string host, path, provider;
    uint16_t port = 0;
    bool listener = false;
    std::optional<srtmux::Endpoint> local;
    srtmux::ProfileConfig profile;
    std::map<std::string, std::string> udp_options;
};
Options parse_arguments(int argc, char** argv);
EndpointOptions parse_endpoint(const std::string&, bool input, const Options&);
std::filesystem::path module_path(const Options&, std::string_view provider);
void print_help();
int64_t number(std::string_view text, int64_t low, int64_t high, std::string_view field);
bool boolean(std::string_view text);
template<class T> T checked(srtmux::Result<T> result) {
    if (auto* error = std::get_if<srtmux::Error>(&result))
        throw std::runtime_error(error->operation + ": " + error->message);
    return std::get<T>(std::move(result));
}
}
