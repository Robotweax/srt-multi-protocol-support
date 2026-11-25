// Output layouts follow apps/statswriter.cpp of Haivision SRT 1.5.7 so existing
// parsers of srt-live-transmit statistics keep working with either provider.
#include "stats.hpp"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace live_transmit {
namespace {
enum Category { general, window, link, send, receive };
// counter: accumulates between native clears, so an interval value is a difference.
struct Row { Category category; const char* json; const char* csv; const char* metric; bool counter; };
// Same rows, order, JSON names and CSV headings as the upstream table. The two
// trailing metrics appear only in the two-column layout.
constexpr Row rows[stat_count] = {
    {general, "time", "Time", "native.msTimeStamp", false},
    {window, "flow", "pktFlowWindow", "native.pktFlowWindow", false},
    {window, "congestion", "pktCongestionWindow", "native.pktCongestionWindow", false},
    {window, "flight", "pktFlightSize", "native.pktFlightSize", false},
    {link, "rtt", "msRTT", "native.msRTT", false},
    {link, "bandwidth", "mbpsBandwidth", "native.mbpsBandwidth", false},
    {link, "maxBandwidth", "mbpsMaxBW", "native.mbpsMaxBW", false},
    {send, "packets", "pktSent", "native.pktSent", true},
    {send, "packetsUnique", "pktSentUnique", "native.pktSentUnique", true},
    {send, "packetsLost", "pktSndLoss", "native.pktSndLoss", true},
    {send, "packetsDropped", "pktSndDrop", "native.pktSndDrop", true},
    {send, "packetsRetransmitted", "pktRetrans", "native.pktRetrans", true},
    {send, "packetsFilterExtra", "pktSndFilterExtra", "native.pktSndFilterExtra", true},
    {send, "bytes", "byteSent", "native.byteSent", true},
    {send, "bytesUnique", "byteSentUnique", "native.byteSentUnique", true},
    {send, "bytesDropped", "byteSndDrop", "native.byteSndDrop", true},
    {send, "byteAvailBuf", "byteAvailSndBuf", "native.byteAvailSndBuf", false},
    {send, "msBuf", "msSndBuf", "native.msSndBuf", false},
    {send, "mbitRate", "mbpsSendRate", "native.mbpsSendRate", false},
    {send, "sendPeriod", "usPktSndPeriod", "native.usPktSndPeriod", false},
    {receive, "packets", "pktRecv", "native.pktRecv", true},
    {receive, "packetsUnique", "pktRecvUnique", "native.pktRecvUnique", true},
    {receive, "packetsLost", "pktRcvLoss", "native.pktRcvLoss", true},
    {receive, "packetsDropped", "pktRcvDrop", "native.pktRcvDrop", true},
    {receive, "packetsRetransmitted", "pktRcvRetrans", "native.pktRcvRetrans", true},
    {receive, "packetsBelated", "pktRcvBelated", "native.pktRcvBelated", true},
    {receive, "packetsFilterExtra", "pktRcvFilterExtra", "native.pktRcvFilterExtra", true},
    {receive, "packetsFilterSupply", "pktRcvFilterSupply", "native.pktRcvFilterSupply", true},
    {receive, "packetsFilterLoss", "pktRcvFilterLoss", "native.pktRcvFilterLoss", true},
    {receive, "bytes", "byteRecv", "native.byteRecv", true},
    {receive, "bytesUnique", "byteRecvUnique", "native.byteRecvUnique", true},
    {receive, "bytesLost", "byteRcvLoss", "native.byteRcvLoss", true},
    {receive, "bytesDropped", "byteRcvDrop", "native.byteRcvDrop", true},
    {receive, "byteAvailBuf", "byteAvailRcvBuf", "native.byteAvailRcvBuf", false},
    {receive, "msBuf", "msRcvBuf", "native.msRcvBuf", false},
    {receive, "mbitRate", "mbpsRecvRate", "native.mbpsRecvRate", false},
    {receive, "msTsbPdDelay", "msRcvTsbPdDelay", "native.msRcvTsbPdDelay", false},
    {general, nullptr, nullptr, "native.pktRcvAvgBelatedTime", false},
    {general, nullptr, nullptr, "native.pktReorderDistance", false},
};
constexpr std::size_t table_rows = 37; // Rows that upstream JSON/CSV print.
enum Index : std::size_t {
    time = 0, flow = 1, congestion = 2, flight = 3, rtt = 4, bandwidth_estimate = 5,
    sent = 7, send_loss = 9, send_drop = 10, retransmitted = 11, send_extra = 12, bytes_sent = 13,
    send_buffer_left = 16, send_rate = 18,
    received = 20, receive_loss = 22, receive_drop = 23, receive_retransmitted = 24, belated = 25,
    receive_extra = 26, receive_supply = 27, receive_filter_loss = 28, bytes_received = 29,
    receive_buffer_left = 33, receive_rate = 35, belated_time = 37, reorder_distance = 38,
};
constexpr const char* category_names[] = {"", "window", "link", "send", "recv"};
std::ostream& operator<<(std::ostream& out, const StatValue& value) {
    switch (value.kind) {
        case StatValue::Kind::unsigned_integer: return out << value.u;
        case StatValue::Kind::signed_integer: return out << value.i;
        default: return out << value.f;
    }
}
StatValue difference(StatValue now, const StatValue& before) {
    if (now.kind == StatValue::Kind::unsigned_integer) now.u = now.u >= before.u ? now.u - before.u : now.u;
    else if (now.kind == StatValue::Kind::signed_integer) now.i = now.i >= before.i ? now.i - before.i : now.i;
    else now.f = now.f >= before.f ? now.f - before.f : now.f;
    return now;
}
std::string timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now(); const std::time_t seconds = system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%FT%T.") << std::setfill('0') << std::setw(6)
        << duration_cast<microseconds>(now.time_since_epoch() - duration_cast<std::chrono::seconds>(now.time_since_epoch())).count()
        << std::put_time(&local, "%z");
    return out.str();
}
}
const std::array<const char*, stat_count>& stat_names() {
    static const std::array<const char*, stat_count> names = [] {
        std::array<const char*, stat_count> out{};
        for (std::size_t i = 0; i < stat_count; ++i) out[i] = rows[i].metric;
        return out;
    }();
    return names;
}
std::string StatsWriter::bandwidth(const NativeStats& now) const {
    std::ostringstream out;
    if (format_ == StatsFormat::json) out << "{\"bandwidth\":" << now.values[bandwidth_estimate] << "}\n";
    else out << "+++/+++SRT BANDWIDTH: " << now.values[bandwidth_estimate] << '\n';
    return out.str();
}
std::string StatsWriter::stats(const NativeStats& now, std::size_t side) {
    auto& previous = previous_[side]; auto& have_previous = have_previous_[side];
    NativeStats shown = now;
    if (interval_ && have_previous && previous.id == now.id) {
        for (std::size_t i = 0; i < stat_count; ++i)
            if (rows[i].counter) shown.values[i] = difference(now.values[i], previous.values[i]);
        // Upstream rates cover the interval since the previous report.
        const double seconds = (now.values[time].number() - previous.values[time].number()) / 1000.0;
        if (seconds > 0) {
            shown.values[send_rate].kind = shown.values[receive_rate].kind = StatValue::Kind::real;
            shown.values[send_rate].f = shown.values[bytes_sent].number() * 8.0 / seconds / 1000000.0;
            shown.values[receive_rate].f = shown.values[bytes_received].number() * 8.0 / seconds / 1000000.0;
        }
    }
    previous = now; have_previous = true;
    const auto& v = shown.values;
    std::ostringstream out;
    if (format_ == StatsFormat::json) {
        const std::string cr = pretty_ ? "\n" : "", tab = pretty_ ? "\t" : "";
        Category category = general;
        out << "{" << cr << tab << "\"sid\":" << shown.id << "," << cr << tab << "\"timepoint\":\"" << timestamp() << "\"";
        for (std::size_t i = 0; i < table_rows; ++i) {
            if (rows[i].category == category) {
                out << "," << cr << tab; if (category != general) out << tab;
            } else {
                if (category != general) out << cr << tab << "}";
                category = rows[i].category;
                out << "," << cr; if (category != general) out << tab;
                out << "\"" << category_names[category] << "\":{" << cr << tab; if (category != general) out << tab;
            }
            out << "\"" << rows[i].json << "\":" << v[i];
        }
        out << cr << tab << "}" << cr << "}" << cr << "\n";
    } else if (format_ == StatsFormat::csv) {
        if (!csv_header_) {
            out << "Timepoint,Time,SocketID";
            for (std::size_t i = 0; i < table_rows; ++i) out << "," << rows[i].csv;
            out << "\n"; csv_header_ = true;
        }
        out << timestamp() << "," << v[time] << "," << shown.id;
        for (std::size_t i = 0; i < table_rows; ++i) out << "," << v[i];
        out << "\n";
    } else {
        using std::setw;
        out << "======= SRT STATS: sid=" << shown.id << "\n";
        out << "PACKETS     SENT: " << setw(11) << v[sent] << "  RECEIVED:   " << setw(11) << v[received] << "\n";
        out << "LOST PKT    SENT: " << setw(11) << v[send_loss] << "  RECEIVED:   " << setw(11) << v[receive_loss] << "\n";
        out << "REXMIT      SENT: " << setw(11) << v[retransmitted] << "  RECEIVED:   " << setw(11) << v[receive_retransmitted] << "\n";
        out << "DROP PKT    SENT: " << setw(11) << v[send_drop] << "  RECEIVED:   " << setw(11) << v[receive_drop] << "\n";
        out << "FILTER EXTRA  TX: " << setw(11) << v[send_extra] << "        RX:   " << setw(11) << v[receive_extra] << "\n";
        out << "FILTER RX  SUPPL: " << setw(11) << v[receive_supply] << "  RX  LOSS:   " << setw(11) << v[receive_filter_loss] << "\n";
        out << "RATE     SENDING: " << setw(11) << v[send_rate] << "  RECEIVING:  " << setw(11) << v[receive_rate] << "\n";
        out << "BELATED RECEIVED: " << setw(11) << v[belated] << "  AVG TIME:   " << setw(11) << v[belated_time] << "\n";
        out << "REORDER DISTANCE: " << setw(11) << v[reorder_distance] << "\n";
        out << "WINDOW      FLOW: " << setw(11) << v[flow] << "  CONGESTION: " << setw(11) << v[congestion] << "  FLIGHT: " << setw(11) << v[flight] << "\n";
        out << "LINK         RTT: " << setw(9) << v[rtt] << "ms  BANDWIDTH:  " << setw(7) << v[bandwidth_estimate] << "Mb/s \n";
        out << "BUFFERLEFT:  SND: " << setw(11) << v[send_buffer_left] << "  RCV:        " << setw(11) << v[receive_buffer_left] << "\n";
    }
    return out.str();
}
}
