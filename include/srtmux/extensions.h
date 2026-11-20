/* Versioned extension ABI. Profiles and native telemetry have a supported subset.
 * See providers/EXTENSIONS.md. Group/hybrid declarations remain proposals. */
#ifndef SRTMUX_EXTENSIONS_H
#define SRTMUX_EXTENSIONS_H
#include <srtmux/provider_abi.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SRTMX_PROFILES "srtmux.profiles"
#define SRTMX_GROUPS "srtmux.groups"
#define SRTMX_TELEMETRY "srtmux.telemetry"
#define SRTMX_HYBRID "srtmux.robotweax.experimental.hybrid"
#define SRTMX_MAJOR 1u
#define SRTMX_MINOR 0u
#define SRTMX_CONTROL 3u
#define SRTMX_SENSOR 4u
#define SRTMX_AES_GCM_REQUIRED 2u
#define SRTMX_KIND_CONNECTION 1u
#define SRTMX_KIND_MESSAGE_CHANNEL 2u
#define SRTMX_KIND_GROUP 3u
#define SRTMX_KIND_LISTENER 4u
#define SRTMX_BROADCAST 1u
#define SRTMX_BACKUP 2u
#define SRTMX_OFF 0u
#define SRTMX_REQUIRED 1u
#define SRTMX_IF_OFFERED 2u
#define SRTMX_MCAST_LEGACY 1u
#define SRTMX_MCAST_INDEPENDENT 2u
#define SRTMX_MCAST_COORDINATED 3u
#define SRTMX_ALREADY_COMMITTED 6

/* Scope every object/member to its runtime. Zero is invalid. No native IDs. */
typedef struct srtmx_object_v1 {
    uint32_t struct_size, kind;
    srtm_handle handle;
} srtmx_object_v1;
typedef struct srtmx_object_info_v1 {
    uint32_t struct_size, state;
    uint64_t revision;
    uint32_t poll_integrated, max_message_bytes;
} srtmx_object_info_v1;
typedef struct srtmx_feature_v1 {
    uint32_t struct_size, native_built, adapter_available, experimental;
    srtm_text constraints, qualification_id, build_fingerprint;
} srtmx_feature_v1;
typedef struct srtmx_fec_v1 {
    uint32_t struct_size, enabled;
    uint32_t columns;
    int32_t rows; /* 1 row, >1 matrix, <-1 column-only. */
    uint32_t layout; /* 1 even, 2 staircase */
    uint32_t arq; /* 0 never, 1 on request, 2 always */
    uint32_t policy; /* 0 ordinary, 1 Robotweax ARQ-first */
    uint32_t tail_probe;
} srtmx_fec_v1;
typedef struct srtmx_tuning_v1 {
    uint32_t struct_size, flow_window_packets;
    int64_t max_bandwidth_bytes_s; /* -1 unlimited, 0 relative, >0 absolute */
    uint64_t input_bandwidth_bytes_s;
    uint32_t overhead_percent, ip_ttl, ip_tos, ipv6_only;
    uint32_t key_refresh_packets, key_preannounce_packets;
    uint32_t send_udp_buffer_bytes, receive_udp_buffer_bytes;
} srtmx_tuning_v1;
typedef struct srtmx_profile_v1 {
    uint32_t struct_size, mode; /* core Live/File or SRTMX_CONTROL/SENSOR */
    srtm_config_v1 base; /* base.mode ignored; all other fields apply */
    uint32_t cipher; /* core disabled/CTR or SRTMX_AES_GCM_REQUIRED */
    uint32_t max_message_bytes; /* whole message, not packet payload ceiling */
    srtmx_fec_v1 fec;
    srtmx_tuning_v1 tuning;
    uint32_t session_auth_required, acp1_required;
    uint32_t path_policy, standby_rtt_policy;
    uint32_t identity_advertisement; /* 0 off, 1 name, 2 name+release */
    /* MPEG-TS null packet deletion: SRTMX_OFF, SRTMX_REQUIRED, or (listeners
     * only) SRTMX_IF_OFFERED. Live only; unsupported providers reject non-OFF. */
    uint32_t null_packet_deletion;
    srtm_text compiled_controller; /* trusted build registry; empty=default */
    srtm_text compiled_filter; /* empty=built-in FEC selection */
} srtmx_profile_v1;
/* cipher is authoritative; base.encryption must be disabled to avoid ambiguity.
 * All nested sizes required. Explicit defaults must be supplied by host builder. */
typedef struct srtmx_admission_request_v1 {
    uint32_t struct_size, proposed_kind;
    srtm_endpoint remote;
    srtm_bytes stream_id;
    srtm_bytes observed_handshake; /* untrusted; callback-duration view */
    uint64_t proposed_peer_group; /* untrusted routing hint, not authorization */
} srtmx_admission_request_v1;
typedef struct srtmx_admission_decision_v1 {
    uint32_t struct_size, accept;
    uint32_t profile_index, rejection_code;
    uint64_t authorized_group_domain; /* trusted application value, nonzero for group */
    uint32_t dispatch_key_size, reserved;
    uint8_t dispatch_key[512];
} srtmx_admission_decision_v1;
typedef void (SRTM_CALL *srtmx_admit_v1)(void *, const srtmx_admission_request_v1 *, srtmx_admission_decision_v1 *);
typedef struct srtmx_listener_v1 {
    uint32_t struct_size, backlog;
    srtm_endpoint local;
    const srtm_handle *allowed_profiles;
    uint32_t profile_count, reserved;
    srtmx_admit_v1 admit;
    void *user_data;
} srtmx_listener_v1;
typedef struct srtmx_message_v1 {
    uint32_t struct_size, in_order;
    int64_t source_time_us;
    int64_t ttl_ms; /* -1 infinite; Control requires -1 */
} srtmx_message_v1;
/* close/release cover extension-owned objects only. Ordinary Connection handles
 * returned by connect/accept continue to use core lifecycle and I/O. */
typedef struct srtmx_profiles_api_v1 {
    srtm_extension_header_v1 header;
    srtm_status (SRTM_CALL *describe)(srtm_runtime *, srtm_text feature, srtmx_feature_v1 *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *create_profile)(srtm_runtime *, const srtmx_profile_v1 *, srtm_handle *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *release_profile)(srtm_runtime *, srtm_handle, srtm_error_v1 *);
    srtm_status (SRTM_CALL *connect)(srtm_runtime *, srtm_handle profile, const srtm_endpoint *local, const srtm_endpoint *remote, uint32_t rendezvous, srtmx_object_v1 *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *listen)(srtm_runtime *, const srtmx_listener_v1 *, srtmx_object_v1 *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *accept)(srtm_runtime *, srtm_handle listener, srtmx_object_v1 *, srtm_io_error_v1 *);
    srtm_status (SRTM_CALL *info)(srtm_runtime *, srtmx_object_v1, srtmx_object_info_v1 *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *close)(srtm_runtime *, srtmx_object_v1, srtm_error_v1 *);
    srtm_status (SRTM_CALL *release)(srtm_runtime *, srtmx_object_v1, srtm_error_v1 *);
    srtm_status (SRTM_CALL *send)(srtm_runtime *, srtm_handle channel, srtm_bytes, const srtmx_message_v1 *, srtm_io_error_v1 *);
    srtm_status (SRTM_CALL *receive)(srtm_runtime *, srtm_handle channel, srtm_mut_bytes, uint64_t *received, srtmx_message_v1 *, srtm_io_error_v1 *);
} srtmx_profiles_api_v1;

typedef struct srtmx_multicast_v1 {
    uint32_t struct_size, mode; /* zero disables */
    srtm_endpoint group, source;
    uint32_t interface_index, ttl;
    uint32_t repair_threshold, reserved; /* 0..16; coordinated requires 0 */
    int64_t max_wire_bytes_s; /* coordinated requires positive, frozen at listen */
} srtmx_multicast_v1;
typedef struct srtmx_group_config_v1 {
    uint32_t struct_size, type;
    srtm_handle profile;
    uint32_t max_members, reserved;
    srtmx_multicast_v1 multicast;
} srtmx_group_config_v1;
typedef struct srtmx_member_config_v1 {
    uint32_t struct_size, weight;
    srtm_endpoint local, remote;
    uint32_t path_id_size, reserved;
    uint8_t path_id[32];
} srtmx_member_config_v1;
typedef struct srtmx_member_v1 {
    uint32_t struct_size, state;
    uint64_t token; /* generation-checked, valid only with owning group */
    uint32_t weight, detached;
    srtm_endpoint local, remote;
    uint32_t path_id_size, reserved;
    uint8_t path_id[32];
} srtmx_member_v1;
typedef struct srtmx_udp_v1 {
    uint32_t struct_size, platform; /* 1 POSIX fd, 2 Winsock SOCKET */
    uint64_t value;
    uint32_t consumed, reserved; /* output even on failed connect */
} srtmx_udp_v1;
typedef struct srtmx_groups_api_v1 {
    srtm_extension_header_v1 header;
    srtm_status (SRTM_CALL *create)(srtm_runtime *, const srtmx_group_config_v1 *, srtmx_object_v1 *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *listen)(srtm_runtime *, const srtmx_group_config_v1 *, const srtmx_listener_v1 *, srtmx_object_v1 *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *add)(srtm_runtime *, srtm_handle group, const srtmx_member_config_v1 *, srtmx_udp_v1 *optional_adoption, uint64_t *member_token, srtm_error_v1 *);
    srtm_status (SRTM_CALL *remove)(srtm_runtime *, srtm_handle group, uint64_t member_token, srtm_error_v1 *);
    srtm_status (SRTM_CALL *members)(srtm_runtime *, srtm_handle group, void *records, uint32_t stride, uint32_t capacity, uint32_t *required, uint64_t *revision, srtm_error_v1 *);
    srtm_status (SRTM_CALL *send)(srtm_runtime *, srtm_handle group, srtm_bytes, const srtmx_message_v1 *, srtm_io_error_v1 *);
    srtm_status (SRTM_CALL *receive)(srtm_runtime *, srtm_handle group, srtm_mut_bytes, uint64_t *received, srtmx_message_v1 *, srtm_io_error_v1 *);
} srtmx_groups_api_v1;

/* Metric IDs are negotiated by name off the data path, never native offsets.
 * Discovery describes units and scope. Values absent in native API stay absent. */
typedef struct srtmx_metric_v1 {
    uint32_t struct_size, id;
    uint32_t value_kind, unit; /* 1 uint64, 2 int64, 3 double; units documented by describe */
    uint64_t member_token;
    union { uint64_t u64; int64_t i64; double f64; } value;
} srtmx_metric_v1;
typedef struct srtmx_metric_info_v1 {
    uint32_t struct_size, id, value_kind, unit;
    srtm_text name, description, unit_name; /* module-owned */
} srtmx_metric_info_v1;
typedef struct srtmx_snapshot_v1 {
    uint32_t struct_size, count;
    uint64_t provider_time_us, clock_domain, counter_epoch, membership_revision;
} srtmx_snapshot_v1;
typedef struct srtmx_telemetry_api_v1 {
    srtm_extension_header_v1 header;
    srtm_status (SRTM_CALL *resolve)(srtm_runtime *, srtm_text name, srtmx_metric_info_v1 *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *snapshot)(srtm_runtime *, srtmx_object_v1, const uint32_t *ids, uint32_t id_count, void *records, uint32_t stride, uint32_t capacity, uint32_t *required, srtmx_snapshot_v1 *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *peer_identity)(srtm_runtime *, srtmx_object_v1, srtm_mut_bytes name, srtm_mut_bytes release, uint32_t *name_required, uint32_t *release_required, srtm_error_v1 *);
} srtmx_telemetry_api_v1;

/* Experimental proposal: requires a new private source-level native bridge.
 * This is NOT a claim that Robotweax exports a hybrid C API. */
typedef struct srtmx_credential_v1 {
    uint32_t struct_size, reserved;
    uint8_t credential_id[16];
    srtm_bytes secret; /* exactly 32 nonzero high-entropy bytes, separate from media key */
    srtm_bytes exact_stream_id;
    uint64_t local_group_id, peer_group_id, policy_generation, expires_provider_us;
} srtmx_credential_v1;
typedef struct srtmx_hybrid_config_v1 {
    uint32_t struct_size, event_capacity, max_operations, reserved;
    uint64_t operation_timeout_us;
} srtmx_hybrid_config_v1;
typedef struct srtmx_hybrid_state_v1 {
    uint32_t struct_size, mode; /* 1 multicast, 2 unicast, 3 overlap, 4 stopped */
    uint64_t revision, incarnation, skipped_messages;
    uint32_t events_lost, automation_paused;
} srtmx_hybrid_state_v1;
typedef struct srtmx_hybrid_event_v1 {
    uint32_t struct_size, kind; /* 1 state, 2 discontinuity, 3 terminal, 4 overflow */
    uint64_t sequence, operation, revision, skipped_messages;
    uint32_t operation_state, reserved; /* 1 pending, 2 committed, 3 cancelled, 4 failed */
} srtmx_hybrid_event_v1;
typedef struct srtmx_operation_v1 {
    uint32_t struct_size, state; /* 1 pending, 2 committed, 3 cancelled, 4 failed */
    uint64_t operation, committed_revision, skipped_messages;
    srtm_io_error_v1 failure;
} srtmx_operation_v1;
typedef struct srtmx_hybrid_api_v1 {
    srtm_extension_header_v1 header;
    srtm_status (SRTM_CALL *attach)(srtm_runtime *, srtm_handle group, const srtmx_hybrid_config_v1 *, srtmx_object_v1 *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *provision)(srtm_runtime *, srtm_handle controller, const srtmx_credential_v1 *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *request)(srtm_runtime *, srtm_handle controller, uint32_t target_mode, uint32_t allow_live_catchup, uint64_t max_skipped_messages, uint64_t *operation, srtm_error_v1 *);
    srtm_status (SRTM_CALL *cancel)(srtm_runtime *, srtm_handle controller, uint64_t operation, srtm_error_v1 *);
    srtm_status (SRTM_CALL *snapshot)(srtm_runtime *, srtm_handle controller, srtmx_hybrid_state_v1 *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *events)(srtm_runtime *, srtm_handle controller, void *records, uint32_t stride, uint32_t capacity, uint32_t *written, srtm_io_error_v1 *);
    srtm_status (SRTM_CALL *operation)(srtm_runtime *, srtm_handle controller, uint64_t operation, srtmx_operation_v1 *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *forget_operation)(srtm_runtime *, srtm_handle controller, uint64_t operation, srtm_error_v1 *);
    srtm_status (SRTM_CALL *resynchronize)(srtm_runtime *, srtm_handle controller, uint64_t observed_revision, srtm_error_v1 *);
} srtmx_hybrid_api_v1;
#define SRTMX_KIND_HYBRID_CONTROLLER 5u
#ifdef __cplusplus
}
#endif
#endif
