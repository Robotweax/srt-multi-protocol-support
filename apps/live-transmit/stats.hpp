#pragma once
// Statistics in the output formats of Haivision's srt-live-transmit (default
// two-column text, JSON, CSV), fed from the provider-neutral native snapshot.
#include <array>
#include <cstdint>
#include <iosfwd>
#include <string>

namespace live_transmit {
// One native counter. Integers stay integers so they print as upstream prints them.
struct StatValue {
    enum class Kind : std::uint8_t { unsigned_integer, signed_integer, real } kind = Kind::signed_integer;
    std::uint64_t u = 0; std::int64_t i = 0; double f = 0;
    double number() const { return kind == Kind::unsigned_integer ? static_cast<double>(u) : kind == Kind::signed_integer ? static_cast<double>(i) : f; }
};
inline constexpr std::size_t stat_count = 39;
// Native metric names, in the order the snapshot is requested and stored.
const std::array<const char*, stat_count>& stat_names();
struct NativeStats {
    std::uint32_t id = 0; // Stands in for the native socket ID, which the facade does not expose.
    std::array<StatValue, stat_count> values{};
};
enum class StatsFormat { columns, json, csv, relay, relay_json };
class StatsWriter {
public:
    // interval: report per-interval counters like upstream without -f; the
    // facade never clears native counters, so the interval is computed here.
    StatsWriter(StatsFormat format, bool pretty, bool interval) : format_(format), pretty_(pretty), interval_(interval) {}
    // side: 0 source, 1 target. Each endpoint keeps its own interval baseline;
    // a new connection has a new id and starts a new one.
    std::string stats(const NativeStats& now, std::size_t side);
    std::string bandwidth(const NativeStats& now) const;
private:
    StatsFormat format_; bool pretty_, interval_, csv_header_ = false;
    bool have_previous_[2] = {false, false};
    NativeStats previous_[2]{};
};
}
