#include "options.hpp"
#include <charconv>
#include <climits>
#include <iostream>
#include <set>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace live_transmit {
int64_t number(std::string_view text, int64_t low, int64_t high, std::string_view field) {
    int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < low || value > high)
        throw std::invalid_argument("Invalid numeric option: " + std::string(field));
    return value;
}
bool boolean(std::string_view text) {
    if (text == "1" || text == "true" || text == "yes" || text == "on") return true;
    if (text == "0" || text == "false" || text == "no" || text == "off") return false;
    throw std::invalid_argument("Invalid boolean option");
}
static std::filesystem::path executable_path() {
    std::vector<char> buffer(4096);
#ifdef __APPLE__
    uint32_t size = static_cast<uint32_t>(buffer.size());
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.resize(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) throw std::runtime_error("Executable location unavailable");
    }
    return std::filesystem::canonical(buffer.data());
#else
    const auto size = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (size <= 0 || static_cast<size_t>(size) == buffer.size()) throw std::runtime_error("Use --provider-dir: executable location unavailable");
    return std::filesystem::path(std::string(buffer.data(), static_cast<size_t>(size)));
#endif
}
Options parse_arguments(int argc, char** argv) {
    Options out;
    std::vector<std::string> positional;
    bool end_options = false;
    for (int i = 1; i < argc; ++i) {
        std::string argument = argv[i];
        if (argument == "--") { end_options = true; continue; }
        if (end_options || argument.empty() || argument == "-" || argument[0] != '-') { positional.push_back(argument); continue; }
        const auto start = argument.starts_with("--") ? 2u : 1u;
        auto delimiter = argument.find_first_of(":=", start);
        auto key = argument.substr(start, delimiter == std::string::npos ? delimiter : delimiter - start);
        auto value = delimiter == std::string::npos ? std::string{} : argument.substr(delimiter + 1);
        const bool flag = key == "h" || key == "help" || key == "version" || key == "q" || key == "quiet" || key == "v" || key == "verbose" ||
            key == "f" || key == "fullstats" || key == "loginternal";
        if (!flag && delimiter == std::string::npos) {
            if (++i == argc) throw std::invalid_argument("Missing value for option: " + key);
            value = argv[i];
        }
        if (flag && delimiter != std::string::npos) throw std::invalid_argument("Flag does not take a value: " + key);
        if (key == "h" || key == "help") out.help = true;
        else if (key == "version") out.version = true;
        else if (key == "q" || key == "quiet") out.quiet = true;
        else if (key == "v" || key == "verbose") out.verbose = true;
        else if (key == "t" || key == "to" || key == "timeout") out.timeout_seconds = static_cast<int>(number(value, 0, 86400, key));
        else if (key == "tm" || key == "timeout-mode") out.timeout_mode = static_cast<int>(number(value, 0, 1, key));
        else if (key == "c" || key == "chunk") out.chunk = static_cast<size_t>(number(value, 1, 65536, key));
        else if (key == "buffering") out.buffering = static_cast<size_t>(number(value, 1, 4096, key));
        else if (key == "a" || key == "auto" || key == "autoreconnect") out.reconnect = boolean(value);
        else if (key == "s" || key == "stats" || key == "stats-report-frequency") out.stats_every = static_cast<size_t>(number(value, 0, INT_MAX, key));
        else if (key == "r" || key == "bwreport" || key == "report" || key == "bandwidth-report" || key == "bitrate-report") out.bandwidth_every = static_cast<size_t>(number(value, 0, INT_MAX, key));
        else if (key == "statsout") out.stats_path = value;
        else if (key == "pf" || key == "statspf") {
            // As upstream: a format name, optionally followed by ",pretty".
            const auto comma = value.find(',');
            const auto extras = comma == std::string::npos ? std::string{} : value.substr(comma + 1);
            out.stats_format = value.substr(0, comma);
            if (out.stats_format != "default" && out.stats_format != "json" && out.stats_format != "csv" && out.stats_format != "relay" && out.stats_format != "relay-json")
                throw std::invalid_argument("Unsupported print format: " + out.stats_format);
            if (!extras.empty() && extras != "pretty") throw std::invalid_argument("Unsupported print format option: " + extras);
            out.stats_pretty = extras == "pretty";
        }
        else if (key == "f" || key == "fullstats") out.full_stats = true;
        // Accepted so existing command lines keep working. The provider
        // interface has no native log control, so they cannot take effect.
        else if (key == "ll" || key == "loglevel" || key == "lfa" || key == "logfa" || key == "logfile" || key == "loginternal") out.logging_requested = true;
        else if (key == "st" || key == "srctime" || key == "sourcetime") {
            if (boolean(value)) throw std::invalid_argument("Source-time forwarding across provider clock domains is not supported");
        }
        else if (key == "provider") out.provider = value;
        else if (key == "provider-dir") out.provider_dir = value;
        else if (key == "haivision-module") out.modules["haivision"] = value;
        else if (key == "robotweax-module") out.modules["robotweax"] = value;
        else if (key == "eof-grace-ms") out.eof_grace_ms = static_cast<int>(number(value, 0, 60000, key));
        else if (key == "coalesce-ms") out.coalesce_ms = static_cast<int>(number(value, 0, 100, key));
        else throw std::invalid_argument("Unsupported command option: " + key);
    }
    if (out.provider != "haivision" && out.provider != "robotweax") throw std::invalid_argument("Provider must be haivision or robotweax");
    if (!out.help && !out.version && positional.size() != 2) throw std::invalid_argument("Specify input and output URIs; use -h for help");
    if (positional.size() == 2) { out.source = positional[0]; out.target = positional[1]; }
    if (out.help || out.version) return out;
    if (out.provider_dir.empty()) {
        const auto directory = executable_path().parent_path();
        const auto build = directory / ".." / "providers";
        out.provider_dir = std::filesystem::is_directory(build) ? build : directory / ".." / "lib" / "srtmux" / "providers";
    }
    out.provider_dir = std::filesystem::absolute(out.provider_dir).lexically_normal();
    for (auto& [name, path] : out.modules) {
        (void)name;
        path = std::filesystem::absolute(path).lexically_normal();
    }
    return out;
}
EndpointOptions parse_endpoint(const std::string& text, bool input, const Options& options) {
    UriParser uri(text);
    EndpointOptions out{};
    out.host = uri.host(); out.path = uri.path(); out.provider = options.provider;
    auto query = uri.parameters();
    if (uri.type() == UriParser::FILE) {
        out.kind = EndpointOptions::Kind::file;
        if (!query.empty() || (!out.host.empty() && out.host != "con" && out.host != "console")) throw std::invalid_argument("Unsupported file URI authority or query");
        if (out.host == "con" || out.host == "console") out.path = "-";
        if (out.path.empty()) throw std::invalid_argument("File URI needs a path or file://con");
        return out;
    }
    if (uri.type() != UriParser::SRT && uri.type() != UriParser::UDP) throw std::invalid_argument("Supported URI schemes: srt, udp, file");
    if (!out.path.empty() || out.host.find('\0') != std::string::npos) throw std::invalid_argument("Invalid network URI path or host");
    out.port = static_cast<uint16_t>(number(uri.port(), 1, 65535, "port"));
    if (uri.type() == UriParser::UDP) {
        out.kind = EndpointOptions::Kind::udp;
        for (const auto& [key, value] : query) {
            if (key != "adapter" && key != "ttl" && key != "tos" && key != "buffer_size" && key != "multicast") throw std::invalid_argument("Unsupported UDP option: " + key);
            out.udp_options[key] = value;
        }
        if (!input && out.host.empty()) throw std::invalid_argument("UDP output needs a host");
        return out;
    }
    out.kind = EndpointOptions::Kind::srt;
    // As in the original, -c is the read size and payloadsize the SRT message
    // limit (default 1316); they are independent settings.
    auto config = srtmux::live_profile();
    out.profile = srtmux::ProfileConfig::basic(config);
    auto& c = out.profile.transport; auto& tuning = out.profile.tuning;
    std::string mode = out.host.empty() ? "listener" : "caller", adapter, bind;
    int local_port = 0;
    for (const auto& [key, value] : query) {
        auto unsigned_value = [&](int64_t low, int64_t high) { return static_cast<uint32_t>(number(value, low, high, key)); };
        if (key == "provider") out.provider = value;
        else if (key == "mode") mode = value;
        else if (key == "adapter") adapter = value;
        else if (key == "bind") bind = value;
        else if (key == "port") local_port = static_cast<int>(number(value, 0, 65535, key));
        else if (key == "transtype") { if (value != "live") throw std::invalid_argument("This application supports Live SRT; reliable File transfer is not implemented"); }
        else if (key == "latency") c.receive_latency = c.peer_latency = std::chrono::milliseconds(unsigned_value(0, INT_MAX));
        else if (key == "rcvlatency") c.receive_latency = std::chrono::milliseconds(unsigned_value(0, INT_MAX));
        else if (key == "peerlatency") c.peer_latency = std::chrono::milliseconds(unsigned_value(0, INT_MAX));
        else if (key == "conntimeo") c.connect_timeout = std::chrono::milliseconds(unsigned_value(1, INT_MAX));
        else if (key == "peeridletimeo") c.peer_idle_timeout = std::chrono::milliseconds(unsigned_value(1, INT_MAX));
        else if (key == "mss") c.mss_bytes = unsigned_value(76, 1500);
        else if (key == "payloadsize") c.payload_bytes = unsigned_value(1, 1456);
        else if (key == "sndbuf") c.send_buffer_bytes = static_cast<uint64_t>(number(value, 1, INT_MAX, key));
        else if (key == "rcvbuf") c.receive_buffer_bytes = static_cast<uint64_t>(number(value, 1, INT_MAX, key));
        else if (key == "streamid") c.stream_id.assign(reinterpret_cast<const std::byte*>(value.data()), reinterpret_cast<const std::byte*>(value.data()+value.size()));
        else if (key == "passphrase") {
            c.passphrase = checked(srtmux::Secret::copy(std::as_bytes(std::span(value)))); c.encryption = srtmux::Encryption::aes_ctr_required;
        }
        else if (key == "pbkeylen") c.key_bits = unsigned_value(16, 32) * 8;
        else if (key == "maxbw") tuning.max_bandwidth_bytes_s = number(value, -1, INT64_MAX, key);
        else if (key == "inputbw") tuning.input_bandwidth_bytes_s = static_cast<uint64_t>(number(value, 0, INT64_MAX, key));
        else if (key == "oheadbw") tuning.overhead_percent = unsigned_value(5, 100);
        else if (key == "fc") tuning.flow_window_packets = unsigned_value(32, INT_MAX);
        else if (key == "ipttl") tuning.ip_ttl = unsigned_value(1, 255);
        else if (key == "iptos") tuning.ip_tos = unsigned_value(0, 255);
        else if (key == "ipv6only") tuning.ipv6_only = boolean(value);
        else if (key == "npd") {
            // Robotweax null packet deletion; the provider rejects it elsewhere.
            if (value == "off") out.profile.null_packet_deletion = srtmux::NullPacketDeletion::off;
            else if (value == "required") out.profile.null_packet_deletion = srtmux::NullPacketDeletion::required;
            else if (value == "if_offered") out.profile.null_packet_deletion = srtmux::NullPacketDeletion::if_offered;
            else throw std::invalid_argument("npd must be off, required or if_offered");
        }
        else if (key == "kmrefreshrate") tuning.key_refresh_packets = unsigned_value(0, INT_MAX);
        else if (key == "kmpreannounce") tuning.key_preannounce_packets = unsigned_value(0, INT_MAX);
        else if (key == "udpsndbuf") tuning.send_udp_buffer_bytes = unsigned_value(1, INT_MAX);
        else if (key == "udprcvbuf") tuning.receive_udp_buffer_bytes = unsigned_value(1, INT_MAX);
        else if (key == "tsbpdmode" || key == "tsbpd" || key == "messageapi" || key == "enforcedencryption") {
            if (!boolean(value)) throw std::invalid_argument("Unsupported disabled SRT option: " + key);
        }
        else throw std::invalid_argument("Unsupported SRT URI option: " + key);
    }
    if (out.provider != "haivision" && out.provider != "robotweax") throw std::invalid_argument("Provider must be haivision or robotweax");
    if (input && options.chunk < c.payload_bytes) throw std::invalid_argument("-c/--chunk must be at least the SRT input payloadsize (default 1316)");
    if (mode != "caller" && mode != "listener" && mode != "default") throw std::invalid_argument("Only caller/listener modes are supported");
    out.listener = mode == "listener" || (mode == "default" && out.host.empty());
    if (out.listener && !c.stream_id.empty()) throw std::invalid_argument("streamid advertises caller metadata; listener admission filtering is unsupported");
    if (!out.listener && out.host.empty()) throw std::invalid_argument("SRT caller needs a host");
    if (!out.listener && out.profile.null_packet_deletion == srtmux::NullPacketDeletion::if_offered)
        throw std::invalid_argument("npd=if_offered is a listener policy; a caller uses npd=required");
    if (c.passphrase && !c.key_bits) c.key_bits = 128;
    if (c.key_bits && !c.passphrase) throw std::invalid_argument("pbkeylen requires a passphrase");
    if (c.key_bits && c.key_bits != 128 && c.key_bits != 192 && c.key_bits != 256) throw std::invalid_argument("pbkeylen must be 16, 24 or 32");
    if (!bind.empty()) {
        if (!adapter.empty() || local_port) throw std::invalid_argument("bind conflicts with adapter/port");
        UriParser local(bind, UriParser::EXPECT_HOST);
        if (!local.scheme().empty() || !local.path().empty() || !local.parameters().empty()) throw std::invalid_argument("Invalid bind endpoint");
        adapter = local.host(); local_port = static_cast<int>(number(local.port(), 0, 65535, "bind port"));
    }
    if (!adapter.empty() || local_port) out.local = srtmux::Endpoint{adapter, static_cast<uint16_t>(local_port), 0};
    return out;
}
std::filesystem::path module_path(const Options& options, std::string_view provider) {
    const auto found = options.modules.find(std::string(provider));
    if (found != options.modules.end()) return found->second;
    return options.provider_dir / ("libsrtmux_" + std::string(provider) + ".so");
}
void print_help() {
    std::cout << R"(Multi-provider port of Haivision srt-live-transmit (SRT 1.5.7 CLI subset)
Usage: srt-live-transmit [options] <input-uri> <output-uri>
  srt://host:port?mode=caller|listener&provider=haivision|robotweax
  ...&npd=required|if_offered      Robotweax MPEG-TS null packet deletion (experimental)
  udp://host:port                  file://con (stdin/stdout), file:///path
  -h, -help, --help                Show supported options
  -version, --version              Show application/provider interface version
  -t:<seconds>, -timeout <seconds> Exit timer (0 disables)
  -tm:0|1                         Whole-run timer / disconnected timer
  -a:yes|no                       Reconnect SRT endpoints (default yes)
  -c:<bytes>                      Max size of data read in one step (default 1456)
  -buffering:<packets>             Packets read per pass (default 10)
  -s:<packets>, -r:<packets>       SRT statistics / bandwidth report frequency
  -statsout:<path>                 Write statistics to a file (default stderr)
  -pf:default|json|csv             Statistics format, as the original
  -pf:relay|relay-json             Relay byte counters with provider names
  -f, -fullstats                   Report totals instead of per-report intervals
  -ll, -lfa, -logfile, -loginternal Accepted for compatibility; not applied
  -q, -v                          Quiet / verbose diagnostics, always stderr
  --provider haivision|robotweax   Default SRT provider (default haivision)
  --provider-dir <directory>       Module directory; no current-directory search
  --haivision-module <path>        Explicit module path
  --robotweax-module <path>        Explicit module path
  --eof-grace-ms <milliseconds>    Best-effort Live EOF grace (default 1000)
  --coalesce-ms <milliseconds>     Batch input wake-ups: less CPU, that much more
                                   delay and burstier output (default 0, off)

Unknown or unsupported options fail explicitly. SRT is Live-only; file endpoints
do not give Live a reliable File-transfer contract. Lost Live tail packets are
allowed. Native log control, rendezvous, groups/FEC/GCM, and source-time
forwarding are not supported by this port.
)";
}
}
