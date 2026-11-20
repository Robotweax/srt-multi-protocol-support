// Included only inside each private adapter. No changes to ordinary I/O lanes.
bool ext_name(srtm_text value, std::string_view expected) {
    return value.data && value.size == expected.size() && std::memcmp(value.data, expected.data(), expected.size()) == 0;
}
srtm_status ext_describe(srtm_runtime* r, srtm_text name, srtmx_feature_v1* out, srtm_error_v1* d) {
    clear(d);
    if (!runtime_ok(r) || !sized(out) || !name.data || name.size > 128) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid feature query", d);
    bool built = false, available = false, experimental = false, known = false;
    for (auto n : {"live", "file", "aes_ctr", "socket_tuning", "native_telemetry"})
        if (ext_name(name, n)) { built = available = known = true; }
    if (ext_name(name, "null_packet_deletion")) {
        known = experimental = true; // Vendor status: experimental, provisional identifiers.
#ifdef SRTMUX_NATIVE_NPD
        built = available = true;
#endif
    }
    for (auto n : {"control", "sensor", "session_auth", "path_ids", "standby_rtt", "arq_first_fec", "multicast", "independent_multicast", "coordinated_multicast"})
        if (ext_name(name, n)) {
            known = true;
#ifndef PROVIDER_HAIVISION
            built = true;
#endif
            experimental = ext_name(name, "standby_rtt") || std::string_view(n).find("multicast") != std::string_view::npos;
        }
    for (auto n : {"aes_gcm", "groups", "acp1", "hybrid", "compiled_controllers", "compiled_filters", "fec", "rendezvous"})
        if (ext_name(name, n)) {
            known = true; built = ext_name(name, "fec") || ext_name(name, "rendezvous");
#ifndef PROVIDER_HAIVISION
            if (ext_name(name, "groups")) built = true;
#endif
            experimental = ext_name(name, "aes_gcm") || ext_name(name, "acp1") || ext_name(name, "hybrid") || std::string_view(n).starts_with("compiled_");
        }
    if (!known) return error(SRTM_ERR_UNSUPPORTED, "Unknown feature", d);
    *out = {out->struct_size, uint32_t(built), uint32_t(available), uint32_t(experimental),
        text("Live/File caller/listener only; no callbacks, rendezvous, groups or message channels; see EXTENSIONS.md"),
        text(""), text(PROVIDER_FINGERPRINT)};
    return SRTM_OK;
}
std::shared_ptr<const ProfileState> ext_profile(srtm_runtime* r, srtm_handle h) {
    if (!runtime_ok(r)) return {};
    std::lock_guard lock(r->control);
    auto it = r->profiles.find(h);
    return it == r->profiles.end() ? nullptr : it->second;
}
bool ext_tune(SRTSOCKET s, const srtmx_tuning_v1& t) {
    auto set = [s](SRT_SOCKOPT opt, auto value) { return srt_setsockflag(s, opt, &value, sizeof(value)) == 0; };
    return set(SRTO_MAXBW, t.max_bandwidth_bytes_s) &&
        (!t.input_bandwidth_bytes_s || set(SRTO_INPUTBW, int64_t(t.input_bandwidth_bytes_s))) &&
        (!t.overhead_percent || set(SRTO_OHEADBW, int(t.overhead_percent))) &&
        (!t.ip_ttl || set(SRTO_IPTTL, int(t.ip_ttl))) &&
        (!t.ip_tos || set(SRTO_IPTOS, int(t.ip_tos))) &&
        (!t.ipv6_only || set(SRTO_IPV6ONLY, 1)) &&
        (!t.key_refresh_packets || set(SRTO_KMREFRESHRATE, int(t.key_refresh_packets))) &&
        (!t.key_preannounce_packets || set(SRTO_KMPREANNOUNCE, int(t.key_preannounce_packets))) &&
        (!t.send_udp_buffer_bytes || set(SRTO_UDP_SNDBUF, int(t.send_udp_buffer_bytes))) &&
        (!t.receive_udp_buffer_bytes || set(SRTO_UDP_RCVBUF, int(t.receive_udp_buffer_bytes)));
}
bool ext_configure(SRTSOCKET s, const ProfileState& p) {
    // Flow control precedes receive-buffer configuration, which depends on it.
    auto fc = int(p.config.tuning.flow_window_packets);
    if (!((!fc || srt_setsockflag(s, SRTO_FC, &fc, sizeof(fc)) == 0) &&
        configure(s, p.config.base) && ext_tune(s, p.config.tuning))) return false;
#ifdef SRTMUX_NATIVE_NPD
    // The policy values are the vendor's own (OFF 0, REQUIRED 1, IF_OFFERED 2).
    // The vendor reserves one payload byte and rejects an oversized payload here.
    if (const int32_t policy = static_cast<int32_t>(p.config.null_packet_deletion);
        policy && srt_setsockflag(s, SRTO_ROBOTWEAX_NPD_POLICY, &policy, sizeof(policy)) != 0) return false;
#endif
    return true;
}
srtm_status ext_create_profile(srtm_runtime* r, const srtmx_profile_v1* c, srtm_handle* out, srtm_error_v1* d) {
    clear(d); if (out) *out = 0;
    if (!runtime_ok(r) || !out || !sized(c) || !sized(&c->base) || !sized(&c->fec) || !sized(&c->tuning) || c->null_packet_deletion > SRTMX_IF_OFFERED || c->base.encryption)
        return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid profile records", d);
    if (c->null_packet_deletion) {
#ifdef SRTMUX_NATIVE_NPD
        if (c->mode != SRTM_LIVE_MESSAGES) return error(SRTM_ERR_UNSUPPORTED, "Null packet deletion requires Live messages", d);
#else
        return error(SRTM_ERR_UNSUPPORTED, "Null packet deletion is not available in this provider", d);
#endif
    }
    if (c->mode != SRTM_LIVE_MESSAGES && c->mode != SRTM_FILE_STREAM)
        return error(SRTM_ERR_UNSUPPORTED, "Message channels are not implemented", d);
    if (c->cipher > SRTM_ENCRYPTION_AES_CTR_REQUIRED || c->fec.enabled || c->fec.columns || c->fec.rows || c->fec.layout || c->fec.arq || c->fec.policy || c->fec.tail_probe ||
        c->session_auth_required || c->acp1_required || c->path_policy || c->standby_rtt_policy || c->identity_advertisement || c->compiled_controller.size || c->compiled_filter.size)
        return error(SRTM_ERR_UNSUPPORTED, "Profile requires an unimplemented feature", d);
    auto base = c->base; base.mode = c->mode; base.encryption = c->cipher;
    if (invalid_config(&base) || (c->mode == SRTM_LIVE_MESSAGES ? c->max_message_bytes != base.payload_bytes : c->max_message_bytes != 0))
        return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid profile transport or message bound", d);
    const auto& t = c->tuning;
    if (t.flow_window_packets > INT_MAX || t.max_bandwidth_bytes_s < -1 || t.input_bandwidth_bytes_s > INT64_MAX ||
        t.overhead_percent > 100 || (t.overhead_percent && t.overhead_percent < 5) || t.ip_ttl > 255 || t.ip_tos > 255 || t.ipv6_only > 1 ||
        t.key_refresh_packets > INT_MAX || t.key_preannounce_packets > INT_MAX || t.send_udp_buffer_bytes > INT_MAX || t.receive_udp_buffer_bytes > INT_MAX)
        return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid tuning range", d);
    auto p = std::make_shared<ProfileState>(); p->config = *c; p->config.base = base;
    if (base.stream_id.size) p->stream.assign(base.stream_id.data, base.stream_id.data + base.stream_id.size);
    if (base.passphrase.size) p->secret.assign(base.passphrase.data, base.passphrase.data + base.passphrase.size);
    p->config.base.stream_id = {p->stream.data(), p->stream.size()};
    p->config.base.passphrase = {p->secret.data(), p->secret.size()};
    p->config.compiled_controller = {}; p->config.compiled_filter = {};
    // Validate native-dependent option combinations transactionally, before publish.
    SocketOwner probe{srt_create_socket()};
    if (probe.value == SRT_INVALID_SOCK || !ext_configure(probe.value, *p)) return native_error(d);
    std::lock_guard lock(r->control);
    if (r->stopping) return error(SRTM_ERR_INVALID_STATE, "Runtime stopping", d);
    if (r->profiles.size() >= 65536) return error(SRTM_ERR_RESOURCE, "Profile capacity exhausted", d);
    // Profiles have their own 48-bit sequence under a disjoint type tag.
    if (r->next_profile >= (UINT64_C(1) << 48)) return error(SRTM_ERR_RESOURCE, "Handle generation exhausted", d);
    const auto h = (uint64_t(PROVIDER_TAG ^ 0x8000u) << 48) | r->next_profile++;
    r->profiles.emplace(h, std::move(p)); *out = h; return SRTM_OK;
}
srtm_status ext_release_profile(srtm_runtime* r, srtm_handle h, srtm_error_v1* d) {
    clear(d); if (!runtime_ok(r)) return error(SRTM_ERR_INVALID_HANDLE, "Invalid runtime", d);
    std::shared_ptr<const ProfileState> retired;
    { std::lock_guard lock(r->control); auto it = r->profiles.find(h);
      if (it == r->profiles.end()) return error(SRTM_ERR_INVALID_HANDLE, "Invalid profile", d);
      retired = std::move(it->second); r->profiles.erase(it); }
    return SRTM_OK;
}
srtm_status ext_connect(srtm_runtime* r, srtm_handle h, const srtm_endpoint* local, const srtm_endpoint* remote, uint32_t rendezvous, srtmx_object_v1* out, srtm_error_v1* d) {
    clear(d);
    if (!sized(out)) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid object output", d);
    out->handle = 0; out->kind = 0;
    auto p = ext_profile(r, h); if (!p) return error(SRTM_ERR_INVALID_HANDLE, "Invalid profile", d);
    if (rendezvous) return error(SRTM_ERR_UNSUPPORTED, "Rendezvous not implemented", d);
    if (p->config.null_packet_deletion == SRTMX_IF_OFFERED) return error(SRTM_ERR_INVALID_ARGUMENT, "Null packet deletion IF_OFFERED is a listener policy", d);
    if (r->stopping) return error(SRTM_ERR_INVALID_STATE, "Runtime stopping", d);
    SocketOwner socket{srt_create_socket()};
    if (socket.value == SRT_INVALID_SOCK || !ext_configure(socket.value, *p)) return native_error(d);
    const auto& c = p->config.base;
    srtm_handle object = 0;
    auto status = publish(r, socket, c.mode, c.payload_bytes, c.encryption, c.mss_bytes, false, false, c.accounting != 0, &object, d);
    if (status != SRTM_OK) return status;
    if (local) status = bind(r, object, local, d);
    if (status == SRTM_OK) status = connect(r, object, remote, d);
    if (status != SRTM_OK && status != SRTM_PENDING) { close(r, object, SRTM_CLOSE_ABORT, 0, nullptr); release(r, object, nullptr); return status; }
    out->kind = SRTMX_KIND_CONNECTION; out->handle = object; return status;
}
srtm_status ext_listen(srtm_runtime* r, const srtmx_listener_v1* c, srtmx_object_v1* out, srtm_error_v1* d) {
    clear(d);
    if (!sized(c) || !sized(out) || c->reserved || !c->allowed_profiles || !c->profile_count || !c->backlog || c->backlog > INT_MAX)
        return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid listener arguments", d);
    out->handle = 0; out->kind = 0;
    if (c->profile_count != 1 || c->admit || c->user_data) return error(SRTM_ERR_UNSUPPORTED, "Admission policies not implemented", d);
    auto p = ext_profile(r, c->allowed_profiles[0]); if (!p) return error(SRTM_ERR_INVALID_HANDLE, "Invalid profile", d);
    const auto& b = p->config.base;
    sockaddr_storage addr{}; auto len = address(&c->local, addr);
    if (!len || (b.mode == SRTM_LIVE_MESSAGES && c->local.family == SRTM_AF_IPV6 && b.payload_bytes > b.mss_bytes-64)) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid listener endpoint", d);
    if (!qualified_address(b.mode, &c->local)) return error(SRTM_ERR_UNSUPPORTED, "Haivision File/IPv6 is not qualified", d);
    if (r->stopping) return error(SRTM_ERR_INVALID_STATE, "Runtime stopping", d);
    SocketOwner socket{srt_create_socket()};
    if (socket.value == SRT_INVALID_SOCK || !ext_configure(socket.value, *p) || srt_bind(socket.value, reinterpret_cast<sockaddr*>(&addr), len) || srt_listen(socket.value, int(c->backlog))) return native_error(d);
    auto status = publish(r, socket, b.mode, b.payload_bytes, b.encryption, b.mss_bytes, true, false, b.accounting != 0, &out->handle, d);
    if (status == SRTM_OK) out->kind = SRTMX_KIND_LISTENER;
    return status;
}
srtm_status ext_accept(srtm_runtime* r, srtm_handle h, srtmx_object_v1* out, srtm_io_error_v1* io) {
    if (!sized(out) || !sized(io)) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid accept output", nullptr, io);
    out->handle = 0; out->kind = 0;
    srtm_error_v1 d{}; d.struct_size = sizeof(d);
    auto status = accept(r, h, &out->handle, &d);
    if (status == SRTM_OK) out->kind = SRTMX_KIND_CONNECTION;
    else if (status == SRTM_FAILED) *io = {io->struct_size, d.category, d.detail_flags, d.vendor_code, d.os_code, d.rejection_code};
    return status;
}
bool ext_object(srtm_runtime* r, srtmx_object_v1 object, bool closed = false) {
    if (!sized(&object) || (object.kind != SRTMX_KIND_CONNECTION && object.kind != SRTMX_KIND_LISTENER)) return false;
    auto* e = raw_lookup(r, object.handle); if (!e) return false;
    std::lock_guard lock(e->lifecycle);
    return raw_lookup(r, object.handle) == e && !e->poller && (closed || !e->closed) && e->listener == (object.kind == SRTMX_KIND_LISTENER);
}
srtm_status ext_info(srtm_runtime* r, srtmx_object_v1 o, srtmx_object_info_v1* out, srtm_error_v1* d) {
    clear(d);
    if (!sized(out)) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid info output", d);
    if (!ext_object(r, o, true)) return error(SRTM_ERR_INVALID_HANDLE, "Invalid typed object", d);
    srtm_connection_info_v1 c{}; c.struct_size = sizeof(c); c.terminal_error.struct_size = sizeof(c.terminal_error);
    auto status = info(r, o.handle, &c, d);
    if (status == SRTM_OK) *out = {out->struct_size, c.state, o.handle, 1, c.max_message_bytes};
    return status;
}
srtm_status ext_close(srtm_runtime* r, srtmx_object_v1 o, srtm_error_v1* d) {
    if (!ext_object(r, o, true)) return error(SRTM_ERR_INVALID_HANDLE, "Invalid typed object", d);
    return close(r, o.handle, SRTM_CLOSE_ABORT, 0, d);
}
srtm_status ext_release(srtm_runtime* r, srtmx_object_v1 o, srtm_error_v1* d) {
    if (!ext_object(r, o, true)) return error(SRTM_ERR_INVALID_HANDLE, "Invalid typed object", d);
    auto status = close(r, o.handle, SRTM_CLOSE_ABORT, 0, d);
    return status == SRTM_OK ? release(r, o.handle, d) : status;
}
struct MetricSpec {
    srtm_text name, description, unit_name;
    uint32_t kind, unit;
    void (*read)(const SRT_TRACEBSTATS&, srtmx_metric_v1&);
};
#include "metrics.inc"
#ifdef SRTMUX_NATIVE_NPD
// Provider-specific metrics follow the common native catalogue and are read
// from the vendor's own cumulative NPD getter, never from SRT_TRACEBSTATS.
struct NpdMetric { srtm_text name, description, unit_name; uint64_t ROBOTWEAX_SRT_NPD_STATS_V1::* counter; bool flag; };
const NpdMetric npd_metrics[] = {
    {text("robotweax.npd.active"), text("1 when null packet deletion was negotiated for this connection"), text("flag"), nullptr, true},
    {text("robotweax.npd.deleted_units"), text("Null packets removed from queued messages"), text("packets"), &ROBOTWEAX_SRT_NPD_STATS_V1::deleted_units, false},
    {text("robotweax.npd.restored_units"), text("Null packets restored into delivered messages"), text("packets"), &ROBOTWEAX_SRT_NPD_STATS_V1::restored_units, false},
    {text("robotweax.npd.malformed_payloads"), text("Payloads rejected by NPD validation"), text("packets"), &ROBOTWEAX_SRT_NPD_STATS_V1::malformed_payloads, false},
};
constexpr uint32_t extra_metrics = static_cast<uint32_t>(std::size(npd_metrics));
#else
constexpr uint32_t extra_metrics = 0;
#endif
constexpr uint32_t metric_total = static_cast<uint32_t>(std::size(metrics)) + extra_metrics;
srtm_status ext_resolve(srtm_runtime* r, srtm_text name, srtmx_metric_info_v1* out, srtm_error_v1* d) {
    clear(d);
    if (!runtime_ok(r) || !sized(out) || !name.data || name.size > 128) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid metric query", d);
    for (uint32_t i = 0; i < std::size(metrics); ++i) if (ext_name(name, {metrics[i].name.data, size_t(metrics[i].name.size)})) {
        const auto& m = metrics[i]; *out = {out->struct_size, i+1, m.kind, m.unit, m.name, m.description, m.unit_name}; return SRTM_OK;
    }
#ifdef SRTMUX_NATIVE_NPD
    for (uint32_t i = 0; i < extra_metrics; ++i) if (ext_name(name, {npd_metrics[i].name.data, size_t(npd_metrics[i].name.size)})) {
        const auto& m = npd_metrics[i]; // Value kind 1 (uint64); unit 1 (packets) as for native packet counters, 0 for the flag.
        *out = {out->struct_size, uint32_t(std::size(metrics))+i+1, 1, m.flag ? 0u : 1u, m.name, m.description, m.unit_name}; return SRTM_OK;
    }
#endif
    return error(SRTM_ERR_UNSUPPORTED, "Unknown metric", d);
}
srtm_status ext_snapshot(srtm_runtime* r, srtmx_object_v1 o, const uint32_t* ids, uint32_t count, void* records,
    uint32_t stride, uint32_t capacity, uint32_t* required, srtmx_snapshot_v1* out, srtm_error_v1* d) {
    clear(d);
    if (!sized(&o) || o.kind != SRTMX_KIND_CONNECTION || !required || !sized(out) || count > metric_total || (count && !ids))
        return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid snapshot arguments", d);
    auto e = lookup(r, o.handle);
    if (!e || e->closed || e->listener || e->poller) return error(SRTM_ERR_INVALID_HANDLE, "Invalid snapshot connection", d);
    for (uint32_t i = 0; i < count; ++i) {
        if (!ids[i] || ids[i] > metric_total) return error(SRTM_ERR_UNSUPPORTED, "Unknown metric ID", d);
        for (uint32_t j = 0; j < i; ++j) if (ids[i] == ids[j]) return error(SRTM_ERR_INVALID_ARGUMENT, "Duplicate metric ID", d);
    }
    *required = count;
    if (capacity < count) return SRTM_BUFFER_TOO_SMALL;
    if (count && (!records || stride < sizeof(srtmx_metric_v1) || uint64_t(stride)*count > SIZE_MAX))
        return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid metric record array", d);
    auto* bytes = static_cast<uint8_t*>(records);
    // memcpy supports caller byte-stride arrays without alignment assumptions.
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t size; std::memcpy(&size, bytes+size_t(i)*stride, sizeof(size));
        if (size < sizeof(srtmx_metric_v1) || size > stride) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid metric record size", d);
    }
    // Each native source is read at most once, and only if a requested ID needs it.
    bool want_native = false, want_extra = false;
    for (uint32_t i = 0; i < count; ++i) (ids[i] <= std::size(metrics) ? want_native : want_extra) = true;
    SRT_TRACEBSTATS stats{};
    if (want_native && srt_bstats(e->socket, &stats, 0) != 0) return native_error(d);
#ifdef SRTMUX_NATIVE_NPD
    ROBOTWEAX_SRT_NPD_STATS_V1 npd{};
    if (want_extra) {
        int size = sizeof(npd);
        if (srt_getsockflag(e->socket, SRTO_ROBOTWEAX_NPD_STATS, &npd, &size) != 0) return native_error(d);
    }
#else
    (void)want_extra;
#endif
    for (uint32_t i = 0; i < count; ++i) {
        auto* dest = bytes+size_t(i)*stride;
        srtmx_metric_v1 m{}; std::memcpy(&m.struct_size, dest, sizeof(m.struct_size));
        m.id = ids[i];
        if (ids[i] <= std::size(metrics)) {
            const auto& spec = metrics[ids[i]-1]; m.value_kind = spec.kind; m.unit = spec.unit;
            spec.read(stats, m);
        }
#ifdef SRTMUX_NATIVE_NPD
        else {
            const auto& spec = npd_metrics[ids[i]-std::size(metrics)-1]; m.value_kind = 1; m.unit = spec.flag ? 0u : 1u;
            m.value.u64 = spec.flag ? npd.active : npd.*spec.counter;
        }
#endif
        std::memcpy(dest, &m, sizeof(m));
    }
    *out = {out->struct_size, count, uint64_t(srt_time_now()), uint64_t(PROVIDER_TAG), o.handle, 0};
    return SRTM_OK;
}
const srtmx_profiles_api_v1 profiles_table = [] {
    srtmx_profiles_api_v1 a{}; a.header = {sizeof(a), 1, 0, 0};
#define X(field, fn) a.field = &Guard<fn>::call
    X(describe, ext_describe); X(create_profile, ext_create_profile); X(release_profile, ext_release_profile);
    X(connect, ext_connect); X(listen, ext_listen); X(accept, ext_accept); X(info, ext_info); X(close, ext_close); X(release, ext_release);
#undef X
    a.send = &Unsupported<decltype(a.send)>::call; a.receive = &Unsupported<decltype(a.receive)>::call;
    return a;
}();
const srtmx_telemetry_api_v1 telemetry_table = [] {
    srtmx_telemetry_api_v1 a{}; a.header = {sizeof(a), 1, 0, 0};
    a.resolve = &Guard<ext_resolve>::call; a.snapshot = &Guard<ext_snapshot>::call;
    a.peer_identity = &Unsupported<decltype(a.peer_identity)>::call;
    return a;
}();
srtm_status extension(srtm_runtime* r, srtm_text name, uint32_t major, uint32_t minor, const srtm_extension_header_v1** out, srtm_error_v1* d) {
    clear(d); if (out) *out = nullptr;
    if (!runtime_ok(r) || !out || !name.data || name.size > 128) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid extension query", d);
    if (!ext_name(name, SRTMX_PROFILES) && !ext_name(name, SRTMX_TELEMETRY)) return error(SRTM_ERR_UNSUPPORTED, "Extension not implemented", d);
    if (major != 1 || minor != 0) return error(SRTM_ERR_ABI_MISMATCH, "Unsupported extension version", d);
    *out = ext_name(name, SRTMX_PROFILES) ? &profiles_table.header : &telemetry_table.header;
    return SRTM_OK;
}
