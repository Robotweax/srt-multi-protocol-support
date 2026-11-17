// Multi-SRT port of Haivision's source -> bounded media queue -> target workflow.
#include "media.hpp"
#include <csignal>
#include <deque>
#include <fstream>
#include <iostream>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void stop(int) { interrupted = 1; }
bool same_file(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code error;
    if (std::filesystem::equivalent(a, b, error)) return true;
    return std::filesystem::weakly_canonical(a) == std::filesystem::weakly_canonical(b);
}
}
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
            struct stat stats_info{};
            if (stat(options.stats_path.c_str(), &stats_info) == 0) {
                if (!S_ISREG(stats_info.st_mode)) throw std::invalid_argument("Statistics output must be a regular file");
                for (int fd : {STDIN_FILENO, STDOUT_FILENO}) {
                    struct stat info{};
                    if (fstat(fd, &info) == 0 && info.st_dev == stats_info.st_dev && info.st_ino == stats_info.st_ino)
                        throw std::invalid_argument("Statistics output aliases a console media descriptor");
                }
            }
        }
        std::signal(SIGINT, stop); std::signal(SIGTERM, stop); std::signal(SIGPIPE, SIG_IGN);
        Providers providers(options);
        auto source = open_medium(std::move(input), true, options, providers);
        auto target = open_medium(std::move(output), false, options, providers);
        std::ofstream stats_file;
        if (!options.stats_path.empty()) {
            stats_file.open(options.stats_path, std::ios::out | std::ios::trunc);
            if (!stats_file) throw std::runtime_error("Cannot open statistics output");
        }
        auto& stats = stats_file.is_open() ? static_cast<std::ostream&>(stats_file) : std::cerr;
        if (!options.quiet) std::cerr << "Relay ready\n";
        std::deque<Packet> queue;
        bool eof = false;
        std::optional<Clock::time_point> eof_since, disconnected_since;
        uint64_t read_bytes = 0, written_bytes = 0, written_packets = 0;
        auto report = [&] {
            const auto seconds = std::chrono::duration<double>(Clock::now() - began).count();
            if (options.stats_format == "json")
                stats << "{\"read_bytes\":" << read_bytes << ",\"written_bytes\":" << written_bytes << ",\"written_packets\":" << written_packets << ",\"application_mbps\":" << written_bytes*8.0/(seconds*1000000) << ",\"source\":" << source->statistics() << ",\"target\":" << target->statistics() << "}\n";
            else stats << "Relay packets=" << written_packets << " input_bytes=" << read_bytes << " output_bytes=" << written_bytes << " application_mbps=" << written_bytes*8.0/(seconds*1000000) << '\n';
            stats.flush(); if (!stats) throw std::runtime_error("Statistics output failed");
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
            bool progress = false;
            while (!eof && source->connected() && queue.size() < options.buffering) {
                auto result = source->read(options.chunk);
                if (result.state == ReadResult::State::would_block) break;
                if (result.state == ReadResult::State::eof) { eof = true; break; }
                read_bytes += result.packet.bytes.size(); queue.push_back(std::move(result.packet)); progress = true;
            }
            while (!queue.empty() && target->connected()) {
                auto& packet = queue.front();
                auto written = target->write(std::span(packet.bytes).subspan(packet.offset));
                if (!written) break;
                packet.offset += *written; written_bytes += *written; progress = true;
                if (packet.offset == packet.bytes.size()) {
                    queue.pop_front(); ++written_packets;
                    if ((options.stats_every && written_packets % options.stats_every == 0) || (options.bandwidth_every && written_packets % options.bandwidth_every == 0)) report();
                } else if (!*written) break;
            }
            if (eof && queue.empty()) {
                const auto drained_at = Clock::now();
                if (!eof_since) eof_since = drained_at;
                if (drained_at - *eof_since >= std::chrono::milliseconds(eof_grace)) break;
            }
            if (!progress) {
                if (eof && queue.empty()) std::this_thread::sleep_for(10ms);
                else if (!queue.empty()) target->wait(10ms);
                else source->wait(10ms);
            }
        }
        if (options.stats_every || options.bandwidth_every || !options.stats_path.empty()) report();
        if (options.verbose && !options.quiet) std::cerr << "Relay stopped: input_bytes=" << read_bytes << " output_bytes=" << written_bytes << " queued_packets=" << queue.size() << '\n';
        return 0;
    } catch (const std::exception& error) {
        if (interrupted) return 0;
        std::cerr << "srt-live-transmit: " << error.what() << '\n'; return 1;
    }
}
