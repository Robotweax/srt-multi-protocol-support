/* Draft v0.2: declarations only; not a released ABI or an installed header. */
#ifndef SRTMUX_PROVIDER_ABI_H
#define SRTMUX_PROVIDER_ABI_H

#include <stdint.h>

#if defined(_WIN32)
# define SRTM_CALL __cdecl
# if defined(SRTM_BUILD_PROVIDER)
#  define SRTM_EXPORT __declspec(dllexport)
# else
#  define SRTM_EXPORT
# endif
#else
# define SRTM_CALL
# define SRTM_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SRTM_ABI_MAJOR 1u
#define SRTM_ABI_MINOR 0u
#define SRTM_INVALID_HANDLE UINT64_C(0)
#define SRTM_WAIT_FOREVER UINT64_MAX

typedef int32_t srtm_status;
#define SRTM_OK                 0
#define SRTM_WOULD_BLOCK        1
#define SRTM_PENDING            2
#define SRTM_END_OF_STREAM      3
#define SRTM_TIMED_OUT          4
#define SRTM_INTERRUPTED        5
#define SRTM_FAILED            -1
#define SRTM_BUFFER_TOO_SMALL  -2

/* Integer constants, never C enum objects, cross this ABI. */
#define SRTM_ERR_NONE             0u
#define SRTM_ERR_INVALID_ARGUMENT 1u
#define SRTM_ERR_UNSUPPORTED      2u
#define SRTM_ERR_INVALID_STATE    3u
#define SRTM_ERR_INVALID_HANDLE   4u
#define SRTM_ERR_AUTHENTICATION   5u
#define SRTM_ERR_PEER_REJECTED    6u
#define SRTM_ERR_CONNECTION_LOST  7u
#define SRTM_ERR_RESOURCE         8u
#define SRTM_ERR_INTERNAL         9u
#define SRTM_ERR_ABI_MISMATCH    10u
#define SRTM_ERR_BUSY            11u
#define SRTM_ERR_TIMEOUT         12u

#define SRTM_CREATED     1u
#define SRTM_BOUND       2u
#define SRTM_CONNECTING  3u
#define SRTM_CONNECTED   4u
#define SRTM_LISTENING   5u
#define SRTM_FAILED_STATE 6u
#define SRTM_CLOSING     7u
#define SRTM_CLOSED      8u

#define SRTM_LIVE_MESSAGES 1u
#define SRTM_FILE_STREAM   2u
#define SRTM_ENCRYPTION_DISABLED 0u
#define SRTM_ENCRYPTION_AES_CTR_REQUIRED 1u
#define SRTM_ACCOUNTING_DISABLED 0u
#define SRTM_ACCOUNTING_PAYLOAD_BYTES 1u
#define SRTM_AF_IPV4 4u
#define SRTM_AF_IPV6 6u

#define SRTM_READY_READ    0x01u
#define SRTM_READY_WRITE   0x02u
#define SRTM_READY_ERROR   0x04u
#define SRTM_READY_STATE   0x08u
#define SRTM_CLOSE_ABORT   0u
#define SRTM_CLOSE_DRAIN   1u

typedef struct srtm_runtime srtm_runtime;
typedef uint64_t srtm_handle;

/* Frozen value types: byte lengths, no NUL terminator required. */
typedef struct srtm_bytes { const uint8_t *data; uint64_t size; } srtm_bytes;
typedef struct srtm_mut_bytes { uint8_t *data; uint64_t size; } srtm_mut_bytes;
typedef struct srtm_text { const char *data; uint64_t size; } srtm_text;

typedef struct srtm_endpoint {
    uint32_t family;
    uint32_t scope_id;
    uint16_t port;             /* Host byte order. */
    uint16_t reserved;
    uint8_t address[16];       /* Network byte order; IPv4 uses first 4 bytes. */
} srtm_endpoint;

/* Set struct_size before every call taking an extensible record. Zero reserved
 * fields. Output sizes are caller capacities; modules never write beyond them.
 * Optional native values use flags, never fabricated sentinel measurements. */
typedef struct srtm_error_v1 {
    uint32_t struct_size;
    uint32_t category;
    uint32_t detail_flags;     /* bit 0 vendor, bit 1 OS, bit 2 rejection valid */
    int32_t vendor_code;
    int32_t os_code;
    int32_t rejection_code;
    uint32_t message_length;   /* Bytes actually written, excluding NUL. */
    uint32_t message_truncated;
    char message[384];         /* UTF-8, NUL terminated, provider copied. */
} srtm_error_v1;

/* Filled only on FAILED for I/O/wait. No string formatting or allocation.
 * detail_flags has the same meaning as srtm_error_v1. */
typedef struct srtm_io_error_v1 {
    uint32_t struct_size;
    uint32_t category;
    uint32_t detail_flags;
    int32_t vendor_code;
    int32_t os_code;
    int32_t rejection_code;
} srtm_io_error_v1;

typedef struct srtm_provider_info_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    srtm_text implementation;  /* "robotweax" or "haivision" */
    srtm_text release;         /* "0.3.3" or "1.5.7" */
    srtm_text source_revision;
    srtm_text build_fingerprint;
    srtm_text target_triple;
    srtm_text build_features;  /* Diagnostic only; query capabilities for policy. */
} srtm_provider_info_v1;

typedef struct srtm_config_v1 {
    uint32_t struct_size;
    uint32_t mode;
    uint32_t accounting;       /* DISABLED adds no adapter payload counters. */
    uint32_t reserved;
    uint32_t encryption;
    uint32_t key_bits;         /* 128/192/256 when encryption required; else 0. */
    uint32_t receive_latency_ms;
    uint32_t peer_latency_ms;
    uint32_t connect_timeout_ms;
    uint32_t peer_idle_timeout_ms;
    uint32_t mss_bytes;
    uint32_t payload_bytes;    /* Live limit; zero for File stream. */
    uint64_t send_buffer_bytes;
    uint64_t receive_buffer_bytes;
    srtm_bytes stream_id;
    srtm_bytes passphrase;     /* Borrowed for call; securely copied if retained. */
} srtm_config_v1;

typedef struct srtm_validation_issue_v1 {
    uint32_t struct_size;
    uint32_t severity;         /* 1 error, 2 peer negotiation requirement */
    char field[64];            /* Stable field name, UTF-8 NUL terminated. */
    srtm_error_v1 detail;
} srtm_validation_issue_v1;

#define SRTM_CAP_AVAILABLE    1u
#define SRTM_CAP_NOT_BUILT    2u
#define SRTM_CAP_UNSUPPORTED  3u
#define SRTM_CAP_EXPERIMENTAL 4u

typedef struct srtm_capability_query_v1 {
    uint32_t struct_size;
    uint32_t role;             /* 1 caller, 2 listener */
    srtm_text feature;         /* Stable names; see README contract. */
    const srtm_config_v1 *config;
} srtm_capability_query_v1;

typedef struct srtm_capability_v1 {
    uint32_t struct_size;
    uint32_t availability;
    uint32_t requires_peer;
    uint32_t reserved;
    srtm_text constraints;     /* Diagnostic prose, not machine-readable policy. */
} srtm_capability_v1;

/* Synchronous admission callback. request and stream_id expire on return.
 * Must not reenter the module. The host's decision buffer is module-owned.
 * V1 admits ordinary connections only; rejection_code is an SRT application
 * rejection code accepted by both adapters. No native socket escapes. */
typedef struct srtm_admission_request_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    srtm_endpoint remote;
    srtm_bytes stream_id;
} srtm_admission_request_v1;

typedef struct srtm_admission_decision_v1 {
    uint32_t struct_size;
    uint32_t accept;           /* 0 reject, 1 accept */
    int32_t rejection_code;
    uint32_t reserved;
} srtm_admission_decision_v1;

typedef void (SRTM_CALL *srtm_admit_fn)(void *user,
    const srtm_admission_request_v1 *, srtm_admission_decision_v1 *);

typedef struct srtm_listener_config_v1 {
    uint32_t struct_size;
    uint32_t backlog;
    srtm_endpoint local;
    srtm_config_v1 transport;
    srtm_admit_fn admit;       /* NULL means accept ordinary connections. */
    void *admit_user;          /* Retained until listener release succeeds. */
} srtm_listener_config_v1;

typedef struct srtm_connection_info_v1 {
    uint32_t struct_size;
    uint32_t state;
    uint32_t valid_fields;    /* bit 0 local, bit 1 remote, bit 2 payload,
                              * bit 3 crypto, bit 4 latencies */
    uint32_t max_message_bytes;
    srtm_endpoint local;
    srtm_endpoint remote;
    uint32_t encryption;
    uint32_t key_bits;
    uint32_t receive_latency_ms;
    uint32_t peer_latency_ms;
    srtm_error_v1 terminal_error; /* Sticky setup/transport failure, if any. */
} srtm_connection_info_v1;

typedef struct srtm_message_info_v1 {
    uint32_t struct_size;
    uint32_t valid_fields;     /* bit 0 source_time_us */
    int64_t source_time_us;    /* This provider runtime's monotonic domain. */
} srtm_message_info_v1;

typedef struct srtm_event_v1 {
    uint32_t struct_size;
    uint32_t readiness;
    srtm_handle object;
    uint64_t registration_token; /* Host token copied on registration/update. */
} srtm_event_v1;

typedef struct srtm_statistics_v1 {
    uint32_t struct_size;
    uint32_t valid_fields;     /* bit 0 accepted TX bytes, bit 1 delivered RX
                              * bytes, bit 2 provider RTT estimate */
    uint64_t counter_epoch;
    uint64_t sample_time_us;   /* Provider runtime clock. */
    uint64_t accepted_tx_bytes; /* Adapter-counted successful payload I/O. */
    uint64_t delivered_rx_bytes;
    uint64_t rtt_estimate_us;  /* Estimate only; no freshness/accuracy claim. */
} srtm_statistics_v1;

typedef struct srtm_log_record_v1 {
    uint32_t struct_size;
    uint32_t severity;         /* 0 trace, 1 debug, 2 info, 3 warn, 4 error */
    srtm_handle object;        /* Zero if not attributable. */
    srtm_text message;         /* Borrowed until callback returns. */
} srtm_log_record_v1;
typedef void (SRTM_CALL *srtm_log_fn)(void *, const srtm_log_record_v1 *);

typedef struct srtm_host_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    srtm_log_fn log;           /* Concurrent, non-reentrant; host queues/copies. */
    void *log_user;            /* Retained until runtime_destroy succeeds. */
} srtm_host_v1;

typedef struct srtm_extension_header_v1 {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t reserved;
} srtm_extension_header_v1;

typedef struct srtm_provider_api_v1 {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t reserved;

    srtm_status (SRTM_CALL *provider_info)(srtm_provider_info_v1 *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *runtime_create)(const srtm_host_v1 *, srtm_runtime **, srtm_error_v1 *);
    srtm_status (SRTM_CALL *runtime_begin_shutdown)(srtm_runtime *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *runtime_shutdown_status)(srtm_runtime *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *runtime_destroy)(srtm_runtime *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *clock_now_us)(srtm_runtime *, uint64_t *, srtm_error_v1 *);

    srtm_status (SRTM_CALL *capability)(srtm_runtime *, const srtm_capability_query_v1 *, srtm_capability_v1 *, srtm_error_v1 *);
    /* count is required array length; capacity zero permits a sizing query.
     * BUFFER_TOO_SMALL is nonmutating; validation never creates sockets. */
    srtm_status (SRTM_CALL *validate)(srtm_runtime *, uint32_t role, const srtm_config_v1 *, srtm_validation_issue_v1 *, uint32_t capacity, uint32_t stride, uint32_t *count, srtm_error_v1 *);

    srtm_status (SRTM_CALL *connection_create)(srtm_runtime *, const srtm_config_v1 *, srtm_handle *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *connection_bind)(srtm_runtime *, srtm_handle, const srtm_endpoint *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *connection_connect)(srtm_runtime *, srtm_handle, const srtm_endpoint *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *connection_info)(srtm_runtime *, srtm_handle, srtm_connection_info_v1 *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *listener_create)(srtm_runtime *, const srtm_listener_config_v1 *, srtm_handle *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *listener_endpoint)(srtm_runtime *, srtm_handle, srtm_endpoint *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *listener_accept)(srtm_runtime *, srtm_handle, srtm_handle *, srtm_error_v1 *);

    /* Data path: no adapter payload copies/allocations, global locks, thread
     * hops, implicit statistics reads or success-path diagnostic writes.
     * Compact error is required; optional failure_detail is written only on
     * FAILED. Neither error record is touched on OK/WOULD_BLOCK/EOF.
     * The same compact-error rule applies to poller_wait (including timeouts).
     * Tables and configuration decisions are cached before steady-state I/O. */
    srtm_status (SRTM_CALL *send_message)(srtm_runtime *, srtm_handle, srtm_bytes, const srtm_message_info_v1 *, srtm_io_error_v1 *, srtm_error_v1 *failure_detail);
    srtm_status (SRTM_CALL *receive_message)(srtm_runtime *, srtm_handle, srtm_mut_bytes, uint64_t *received, srtm_message_info_v1 *, srtm_io_error_v1 *, srtm_error_v1 *failure_detail);
    srtm_status (SRTM_CALL *write_stream)(srtm_runtime *, srtm_handle, srtm_bytes, uint64_t *accepted, srtm_io_error_v1 *, srtm_error_v1 *failure_detail);
    srtm_status (SRTM_CALL *read_stream)(srtm_runtime *, srtm_handle, srtm_mut_bytes, uint64_t *received, srtm_io_error_v1 *, srtm_error_v1 *failure_detail);
    srtm_status (SRTM_CALL *statistics)(srtm_runtime *, srtm_handle, srtm_statistics_v1 *, srtm_error_v1 *);

    srtm_status (SRTM_CALL *object_close)(srtm_runtime *, srtm_handle, uint32_t close_mode, uint64_t drain_timeout_ms, srtm_error_v1 *);
    srtm_status (SRTM_CALL *object_close_status)(srtm_runtime *, srtm_handle, srtm_error_v1 *);
    srtm_status (SRTM_CALL *object_release)(srtm_runtime *, srtm_handle, srtm_error_v1 *);

    srtm_status (SRTM_CALL *poller_create)(srtm_runtime *, srtm_handle *, srtm_error_v1 *);
    srtm_status (SRTM_CALL *poller_register)(srtm_runtime *, srtm_handle poller, srtm_handle object, uint32_t interests, uint64_t token, srtm_error_v1 *);
    srtm_status (SRTM_CALL *poller_remove)(srtm_runtime *, srtm_handle poller, srtm_handle object, srtm_error_v1 *);
    srtm_status (SRTM_CALL *poller_wait)(srtm_runtime *, srtm_handle, uint64_t timeout_ms, srtm_event_v1 *, uint32_t capacity, uint32_t stride, uint32_t *count, srtm_io_error_v1 *, srtm_error_v1 *failure_detail);
    srtm_status (SRTM_CALL *poller_wake)(srtm_runtime *, srtm_handle, srtm_error_v1 *);
    srtm_status (SRTM_CALL *poller_release)(srtm_runtime *, srtm_handle, srtm_error_v1 *);

    /* Optional immutable table: NULL on unavailable/incompatible extension.
     * No groups/native handles/extensions are implicitly part of ABI v1. */
    srtm_status (SRTM_CALL *query_extension)(srtm_runtime *, srtm_text name, uint32_t major, uint32_t min_minor, const srtm_extension_header_v1 **, srtm_error_v1 *);
} srtm_provider_api_v1;

/* Each module exports exactly this symbol, resolved from that module's handle.
 * The modules are never directly linked into the host's symbol namespace.
 * Success returns an immutable module-owned table valid until process exit.
 * Unsupported major/minimum minor fails; no partial runtime initialization.
 * All functions in a negotiated v1.0 table are mandatory. */
SRTM_EXPORT srtm_status SRTM_CALL srtm_provider_get_api(
    uint32_t requested_major, uint32_t minimum_minor,
    const srtm_provider_api_v1 **out_api, srtm_error_v1 *error);

#ifdef __cplusplus
}
#endif
#endif
