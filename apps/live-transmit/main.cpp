// Multi-SRT port of Haivision's srt-live-transmit relay loop: read a burst from
// the source, hand each packet to the target, and count what the target could
// not take as lost. Live data is never held back for a slow or absent target.
#include "media.hpp"
#include <csignal>
#include <ctime>
#include <fstream>
#include <iostream>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
volatile std::sig_atomic_t interrupted = 0;
void stop(int) { interrupted = 1; }
// Shown instead of the full URI: queries can carry passphrases and stream IDs.
std::string without_query(const std::string& uri) { return uri.substr(0, uri.find('?')); }
bool same_file(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code error;
    if (std::filesystem::equivalent(a, b, error)) return true;
    return std::filesystem::weakly_canonical(a) == std::filesystem::weakly_canonical(b);
}
}
bool live_transmit::stop_requested() noexcept { return interrupted != 0; }
int main(int argc, char** argv) {
    using namespace live_transmit;
    using namespace std::chrono_literals;
    try {
        const auto options = parse_arguments(argc, argv);
        if (options.help) { print_help(); return 0; }
        if (options.version) { std::cout << "srt-live-transmit multi-SRT 0.1.0 (Haivision 1.5.7 CLI port; provider ABI v1)\n"; return 0; }
        const auto began = Clock::now();
        auto input = parse_endpoint(options.source, true, options);
        auto output = parse_endpoint(options.target, false, options);
        const auto eof_grace = output.kind == EndpointOptions::Kind::srt ? options.eof_grace_ms : 0;
        if (input.kind == EndpointOptions::Kind::file && output.kind == EndpointOptions::Kind::file && input.path != "-" && output.path != "-" && same_file(input.path, output.path))
            throw std::invalid_argument("Input and output refer to the same file");
        for (const auto* endpoint : {&input, &output}) {
            if (!options.stats_path.empty() && endpoint->kind == EndpointOptions::Kind::file && endpoint->path != "-" && same_file(endpoint->path, options.stats_path))
                throw std::invalid_argument("Statistics and media must use different files");
        }
        if (!options.stats_path.empty()) {
#ifdef _WIN32
            if (std::filesystem::exists(options.stats_path)) {
                if (!std::filesystem::is_regular_file(options.stats_path)) throw std::invalid_argument("Statistics output must be a regular file");
                HANDLE stats_handle = CreateFileW(std::filesystem::path(options.stats_path).c_str(), 0,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (stats_handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot inspect statistics file");
                BY_HANDLE_FILE_INFORMATION stats_info{};
                bool inspected = GetFileInformationByHandle(stats_handle, &stats_info) != 0;
                CloseHandle(stats_handle);
                if (!inspected) throw std::runtime_error("Cannot identify statistics file");
                for (auto id : {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE}) {
                    BY_HANDLE_FILE_INFORMATION info{};
                    if (GetFileInformationByHandle(GetStdHandle(id), &info) &&
                        info.dwVolumeSerialNumber == stats_info.dwVolumeSerialNumber &&
                        info.nFileIndexHigh == stats_info.nFileIndexHigh && info.nFileIndexLow == stats_info.nFileIndexLow)
                        throw std::invalid_argument("Statistics output aliases a console media descriptor");
                }
            }
#else
            struct stat stats_info{};
            if (stat(options.stats_path.c_str(), &stats_info) == 0) {
                if (!S_ISREG(stats_info.st_mode)) throw std::invalid_argument("Statistics output must be a regular file");
                for (int fd : {STDIN_FILENO, STDOUT_FILENO}) {
                    struct stat info{};
                    if (fstat(fd, &info) == 0 && info.st_dev == stats_info.st_dev && info.st_ino == stats_info.st_ino)
                        throw std::invalid_argument("Statistics output aliases a console media descriptor");
                }
            }
#endif
        }
        std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
#ifdef _WIN32
        WSADATA winsock{};
        if (WSAStartup(MAKEWORD(2, 2), &winsock)) throw std::runtime_error("Winsock startup failed");
        struct WinsockCleanup { ~WinsockCleanup() { WSACleanup(); } } cleanup;
#else
        std::signal(SIGPIPE, SIG_IGN);
#endif
        Providers providers(options);
        auto source = open_medium(std::move(input), true, options, providers);
        auto target = open_medium(std::move(output), false, options, providers);
        std::ofstream stats_file;
        if (!options.stats_path.empty()) {
            stats_file.open(options.stats_path, std::ios::out | std::ios::trunc);
            if (!stats_file) throw std::runtime_error("Cannot open statistics output");
        }
        auto& stats = stats_file.is_open() ? static_cast<std::ostream&>(stats_file) : std::cerr;
        if (options.logging_requested && !options.quiet) std::cerr << "Note: native logging options are accepted for compatibility but not applied by this port\n";
        // -pf default/json/csv report each SRT endpoint in the original's
        // formats; the relay formats report the relay's own byte counters.
        const bool relay_format = options.stats_format == "relay" || options.stats_format == "relay-json";
        const auto format = options.stats_format == "json" ? StatsFormat::json : options.stats_format == "csv" ? StatsFormat::csv : StatsFormat::columns;
        StatsWriter writer(format, options.stats_pretty, !options.full_stats);
        size_t endpoint_packets[2] = {0, 0};
        // As upstream: each SRT endpoint reports every N of its own packets.
        auto endpoint_report = [&](Medium& medium, size_t side) {
            const auto counter = endpoint_packets[side]++;
            const bool bandwidth = options.bandwidth_every && counter % options.bandwidth_every == options.bandwidth_every - 1;
            const bool statistics = !relay_format && options.stats_every && counter % options.stats_every == options.stats_every - 1;
            if (!bandwidth && !statistics) return;
            NativeStats now;
            if (!medium.native_stats(now)) return;
            if (bandwidth) std::cerr << writer.bandwidth(now) << std::flush;
            if (statistics) { stats << writer.stats(now, side) << std::flush; if (!stats) throw std::runtime_error("Statistics output failed"); }
        };
        if (!options.quiet) std::cerr << "Media path: '" << without_query(options.source) << "' --> '" << without_query(options.target) << "'\n";
        // A file is cut to what one target message can carry; network input is
        // read with the full chunk and anything the target cannot carry is lost.
        const size_t limit = target->message_limit();
        const size_t read_size = source->pausable() && limit && limit < options.chunk ? limit : options.chunk;
        bool eof = false, oversize_reported = false;
        std::vector<std::byte> buffer(read_size); // Reused for every read.
        std::optional<std::vector<std::byte>> held; // Only from a pausable source, only while the target is full.
        std::optional<Clock::time_point> eof_since, disconnected_since;
        uint64_t read_bytes = 0, written_bytes = 0, written_packets = 0, lost_bytes = 0, reported_lost_bytes = 0;
        std::time_t loss_report_at = std::time(nullptr);
        auto report = [&] {
            const auto seconds = std::chrono::duration<double>(Clock::now() - began).count();
            if (options.stats_format == "relay-json")
                stats << "{\"read_bytes\":" << read_bytes << ",\"written_bytes\":" << written_bytes << ",\"lost_bytes\":" << lost_bytes << ",\"written_packets\":" << written_packets << ",\"application_mbps\":" << written_bytes*8.0/(seconds*1000000) << ",\"source\":" << source->statistics() << ",\"target\":" << target->statistics() << "}\n";
            else stats << "Relay packets=" << written_packets << " input_bytes=" << read_bytes << " output_bytes=" << written_bytes << " lost_bytes=" << lost_bytes << " application_mbps=" << written_bytes*8.0/(seconds*1000000) << '\n';
            stats.flush(); if (!stats) throw std::runtime_error("Statistics output failed");
        };
        // Returns false only when a pausable source should hold this packet.
        auto deliver = [&](std::span<const std::byte> packet) {
            const auto size = packet.size();
            const auto result = target->connected() ? target->write(packet) : Write::blocked;
            if (result == Write::done) {
                written_bytes += size; ++written_packets;
                endpoint_report(*target, 1);
                if (relay_format && options.stats_every && written_packets % options.stats_every == 0) report();
                return true;
            }
            if (result == Write::blocked && source->pausable() && !stop_requested()) return false; // Nothing is lost by pausing a file.
            lost_bytes += size; return true;
        };
        while (!interrupted) {
            const auto now = Clock::now();
            source->tick(); target->tick();
            if (source->ended()) eof = true;
            if (target->ended()) break;
            const bool connected = source->connected() && target->connected();
            if (connected) disconnected_since.reset();
            else if (!disconnected_since) disconnected_since = now;
            if (options.timeout_seconds) {
                const auto origin = options.timeout_mode == 0 ? std::optional(began) : disconnected_since;
                if (origin && now - *origin >= std::chrono::seconds(options.timeout_seconds)) break;
            }
            size_t burst = 0;
            bool drained = false; // The source reported would-block: nothing more to read now.
            if (held) {
                if (!deliver(*held)) { target->wait(std::chrono::milliseconds(10)); continue; }
                held.reset(); ++burst;
            }
            // Deplete the source in bursts of up to -buffering packets per pass.
            while (!eof && !held && source->connected() && burst < options.buffering && (target->connected() || !source->pausable())) {
                const auto result = source->read(buffer);
                if (result.state == ReadResult::State::would_block) { drained = true; break; }
                if (result.state == ReadResult::State::eof) { eof = true; break; }
                ++burst;
                if (result.state == ReadResult::State::dropped) {
                    read_bytes += result.bytes; lost_bytes += result.bytes;
                    if (!oversize_reported && !options.quiet) { std::cerr << "Input larger than the chunk size was dropped; increase -c to carry it\n"; oversize_reported = true; }
                    continue;
                }
                read_bytes += result.bytes;
                endpoint_report(*source, 0);
                const std::span<const std::byte> packet(buffer.data(), result.bytes);
                if (!deliver(packet)) held.emplace(packet.begin(), packet.end());
            }
            if (!options.quiet && lost_bytes != reported_lost_bytes) {
                const auto wall = std::time(nullptr);
                if (std::difftime(wall, loss_report_at) >= 5.0) {
                    std::cerr << lost_bytes << " bytes lost, " << written_bytes << " bytes sent, " << read_bytes << " bytes received\n";
                    loss_report_at = wall; reported_lost_bytes = lost_bytes;
                }
            }
            if (eof && !held) {
                const auto drained_at = Clock::now();
                if (!eof_since) eof_since = drained_at;
                if (drained_at - *eof_since >= std::chrono::milliseconds(eof_grace)) break;
                std::this_thread::sleep_for(10ms); continue;
            }
            // Wait on the source only: it alone gates progress. The short cap
            // lets a reconnecting endpoint advance; steady state wakes rarely.
            // A drained source is waited on at once instead of being asked again.
            // Optional coalescing: after emptying the source, stay away from it
            // for a moment so the next wake-up finds several packets. One
            // wake-up per packet is the larger part of this relay's CPU cost;
            // the price is up to that much added delay and burstier output.
            if (options.coalesce_ms && burst && drained) std::this_thread::sleep_for(std::chrono::milliseconds(options.coalesce_ms));
            if (!burst || drained) source->wait(connected ? 100ms : 10ms);
        }
        if (relay_format && (options.stats_every || !options.stats_path.empty())) report();
        if (options.verbose && !options.quiet) std::cerr << "Relay stopped: input_bytes=" << read_bytes << " output_bytes=" << written_bytes << " lost_bytes=" << lost_bytes << '\n';
        return 0;
    } catch (const std::exception& error) {
        if (interrupted) return 0;
        std::cerr << "srt-live-transmit: " << error.what() << '\n'; return 1;
    }
}
