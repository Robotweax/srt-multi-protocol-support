# Native-to-wrapper audit, 2026-11-16

**Robotweax's additional functionality is not yet fully supported by the wrapper.**
This historical audit records the runtime at `8f3579c`; see
[the current implementation status](../../../providers/EXTENSIONS.md) for subsequent
profile and telemetry work. At that audited revision, the adjacent headers propose future
interfaces and do not change that answer. Both modules then returned
Unsupported from `query_extension`. Enum names, header declarations and native
capabilities are not evidence of implemented wrapper support.

## Coverage and evidence

The [symbol/option ledger](NATIVE-SURFACE.md) and [machine-readable inventory](native-surface.json)
cover all 86 Robotweax exported names, all 74 Haivision annotated public names,
94 Robotweax socket options and 61 Haivision options, including conditional
build branches. All Haivision names in this inventory occur in Robotweax's
compatibility surface; that does not prove equivalent semantics. The inventory
also records message/statistics/Robotweax result fields, transport profiles,
compatibility-manifest classifications and 32 source-tree C++ headers.
`internal_mapping` means the adapter uses an operation, not that applications
can exercise its entire native contract. `srt_setlogstream` is a commented-out
proposal, not a missing exported declaration. `srt_bind_peerof` is an inline alias.

This is exhaustive public C surface coverage and a feature-family disposition
of source-only C++. It is not a promise to wrap every private C++ method or a
new behavioral conformance test. The latter headers are not installed by the
native build; internal protocol/session/crypto/controller classes stay private.
All source hashes and exact revisions are recorded. Verify with:

```sh
python3 tools/check_native_audit.py --robotweax /path/to/pinned/robotweax --haivision /path/to/pinned/haivision
```

Primary sources, inspected from the pinned trees:

- [Robotweax public header](https://github.com/Robotweax/srt/blob/9b315a721eff0b0e7186fa7f97886dbd94abdb00/include/srt/srt.h), [toolkit header](https://github.com/Robotweax/srt/blob/9b315a721eff0b0e7186fa7f97886dbd94abdb00/include/robotweax_srt.h), [export manifest](https://github.com/Robotweax/srt/blob/9b315a721eff0b0e7186fa7f97886dbd94abdb00/cmake/public-symbols.txt), [compatibility manifest](https://github.com/Robotweax/srt/blob/9b315a721eff0b0e7186fa7f97886dbd94abdb00/compat/srt-1.5.7-api.json).
- [Haivision public header](https://github.com/Haivision/srt/blob/899348d8318eb9a3c5a5b6ec43c4a1114288773a/srtcore/srt.h).
- [Group contracts](https://github.com/Robotweax/srt/blob/9b315a721eff0b0e7186fa7f97886dbd94abdb00/docs/connection-groups.md), [FEC/Sensor](https://github.com/Robotweax/srt/blob/9b315a721eff0b0e7186fa7f97886dbd94abdb00/docs/packet-filter.md), [Control](https://github.com/Robotweax/srt/blob/9b315a721eff0b0e7186fa7f97886dbd94abdb00/docs/control-profile.md).
- [Session authentication](https://github.com/Robotweax/srt/blob/9b315a721eff0b0e7186fa7f97886dbd94abdb00/docs/session-authentication.md), [encryption](https://github.com/Robotweax/srt/blob/9b315a721eff0b0e7186fa7f97886dbd94abdb00/docs/encryption.md), [ACP1](https://github.com/Robotweax/srt/blob/9b315a721eff0b0e7186fa7f97886dbd94abdb00/docs/experimental-acp1.md).
- [Path identifiers](https://github.com/Robotweax/srt/blob/9b315a721eff0b0e7186fa7f97886dbd94abdb00/docs/group-path-identifiers.md), [standby RTT](https://github.com/Robotweax/srt/blob/9b315a721eff0b0e7186fa7f97886dbd94abdb00/docs/standby-rtt-foundation.md), [ARQ-first FEC](https://github.com/Robotweax/srt/blob/9b315a721eff0b0e7186fa7f97886dbd94abdb00/docs/arq-first-fec.md).
- [Hybrid control](https://github.com/Robotweax/srt/blob/9b315a721eff0b0e7186fa7f97886dbd94abdb00/docs/experimental-hybrid-control.md), [controllers](https://github.com/Robotweax/srt/blob/9b315a721eff0b0e7186fa7f97886dbd94abdb00/docs/experimental-controllers.md), [filters](https://github.com/Robotweax/srt/blob/9b315a721eff0b0e7186fa7f97886dbd94abdb00/docs/experimental-filters.md).

## Capability matrix

“Missing” below means absent from the wrapper, even if native source implements
it. Proposed table names refer to [the design](README.md), not available APIs.

| Feature | Native scope | Wrapper today | Disposition |
| --- | --- | --- | --- |
| Live messages, File streams, caller/listener, IPv4 | Both | Implemented basic subset | Keep core fast path |
| IPv6 | Both | Live supported; Haivision File rejected | Separate qualification before enabling File/IPv6 |
| AES-CTR 128/192/256 | Both | Configurable; local performance qualification covers 128 | Core; no claim all key sizes qualified |
| Polling and concurrent abort-close | Both native mechanisms differ | Implemented provider-local lifecycle | Preserve existing rules |
| Drain-close, native file helpers, legacy UDT aliases, raw system-socket polling | Common native surface | Missing or intentionally excluded | Drain needs separate contract; file helpers/aliases/raw OS event APIs stay outside transport core |
| Rendezvous, local bind selection, socket tuning, key rotation tuning | Common surface, differing combinations | Missing beyond core typed subset | Profiles; retain controlled nonblocking settings |
| Full message controls and native statistics | Common, with layout differences | Source time and wrapper payload counters only | Profiles + telemetry; native 82-field statistics are not current wrapper statistics |
| FEC and AES-GCM | Both have APIs; build/profile limitations | Missing; AEAD preview off in reference builds | Profiles; neither is exclusively Robotweax |
| Broadcast/weighted Backup groups | Both have group APIs; Haivision bonding build off | Missing | Groups; no balancing or striping promise |
| Listener child-policy selection, bonded accept, connect callbacks | Common hooks, RW adds policies | Missing; core callback slots unsupported | Admission + tagged accept; connect completion stays poll-driven |
| Native logging configuration/callbacks | Common | Missing | Separate future diagnostics contract, never synchronous payload-path logging by default |
| Control profile `control-v1` | RW only; reliable ordered Message API over FileCC | Missing | Dedicated MessageChannel; no finite TTL, TSBPD, late drop or FEC; no exactly-once application execution guarantee |
| Sensor profile `fec-sensor-v1` | RW only; fixed 4-source row, no ARQ/TSBPD | Missing | Dedicated MessageChannel; bounded one-sample messages, loss/expiry allowed |
| Session authentication v2 | RW-only bilateral; passphrase + enforced encryption | Missing | Profiles/admission; no silent fallback or Haivision interoperability when required |
| ACP1 runtime-control authentication | RW experimental, explicit build flag off | Missing | Independent experimental feature gate; not synonymous with session authentication |
| Group authorization domain / peer group observation | RW | Missing | Admission; local trusted nonzero domain, never infer authorization from peer IDs |
| Group path IDs/policies | RW Broadcast/Backup Live caller/listener | Missing | Groups/profiles; opaque 1–32 bytes, not authenticated or proof of delivery path |
| Standby RTT negotiation/measurements | RW experimental Backup Live TSBPD | Missing | Profiles/telemetry; monitoring only, not scheduler or source-health proof |
| ARQ-first matrix FEC and tail probe | RW sender-local Live TSBPD LiveCC, individual caller/listener | Missing | Profiles; rows >=2 and ARQ always; reject groups/RDV/File/Control/Sensor/custom filters |
| Identity advertisement, peer identity, observed handshake | RW opt-in/provisional identity | Missing | Profiles/admission/telemetry; self-reported identity and observed handshake are not verified capabilities |
| Legacy multicast dispatch and repair aggregation | RW experimental group extension | Missing | Groups; explicit dispatch authorization and repair threshold, no automatic fallback |
| Independent multicast source | RW IPv4 only; concrete listener source before bind | Missing | Groups; single endpoint receiver, no bonding/address rewriting |
| Coordinated multicast sending | RW opt-in independent IPv4 listener sender | Missing | Groups; positive frozen wire-rate, 64-frame native queue, <=64 receiver slots including detached |
| Group send authority/path, RTT/standby RTT/multicast statistics | RW C queries/options | Missing | Telemetry; tokens replace native socket IDs |
| Hybrid multicast/unicast fallback and return | RW experimental internal C++; requires ACP1 build | Missing; no native public hybrid C API | Separate experimental hybrid proposal and private source bridge; not releasable from header presence alone |
| Custom controllers / custom packet filters | RW experimental source-compiled registries, off by default | Missing | Select trusted versioned build entries at setup; no arbitrary runtime plugin ABI |
| Crypto backend, mobile pacing, Windows timer choices | Native build/platform choices | No runtime selection | Build fingerprint/capability metadata; never mutable socket options |
| Packet inspector and standalone options value object | RW toolkit | Not exposed | Deliberately outside transport ABI; no connection ownership or I/O semantics |
| Internal protocol/crypto/session helpers and sample media workflows | RW source-only C++/examples | Not exposed | Remain provider-private or application-level; exact-source rebuild required |

## Constraints that change the design

Session authentication authenticates KM and FEC parity, not all handshake/control
fields or CTR DATA, and is not forward secrecy. ACP1 is separate experimental
control authentication. GCM availability is build-dependent; broad cross-provider,
key-size, rotation and group interoperability cannot be inferred from AES-128
Live evidence. Shared multicast keys do not authenticate members to each other;
removing a member does not revoke a key it already knows.

Robotweax FEC accepts columns 2..128; do not assume Haivision's larger parameter
range. Native listener-only ordinary FEC can accept a caller without FEC: a
wrapper “required” FEC policy must verify admission/negotiation or reject the
configuration if it cannot enforce it. Sensor requires its matching bundle.
ARQ-first documentation includes narrowly scoped latency experiments; the public
header's 120-ms qualification wording must not become a general latency guarantee.
Public statistics have FEC packet counters, not separate FEC-byte or internal
parity-withheld/tail-probe counters.

Coordinated multicast send succeeds on bounded queue acceptance, not UDP or
remote delivery. It refuses packet filters, session-authentication profiles and
fragmented messages. Repair threshold must be zero. Its shared unicast repair
budget is native four-times-MAXBW with one MTU burst, not an application-selectable
arbitrary rate. Detached members consume receiver slots. Ordinary multicast's
repair budget and ordinary per-member group bandwidth have different accounting.

Control's whole-command bound depends on sender and peer windows; packet payload
size is not that bound. Native receive truncation must be resolved before promising
atomic short-buffer behavior. Sensor is inappropriate for reliable commands.

## Build and qualification are separate axes

Reference builds disable AEAD preview, Haivision bonding and MAXREXMITBW. Robotweax
experimental ACP1/controllers/filters/hybrid/mobile pacing default off. A future
feature build must fingerprint these switches, crypto backend and compiled plugin
sources; today's fingerprint is insufficient to certify all such variants.
Native built, adapter exposed, experimental status and tested deployment must be
reported separately. Never label a missing adapter feature “available” because
its vendor header declares an option.

Existing local IPv4 File/Live/plaintext/AES-128 performance evidence remains valid
for its recorded scope. None of the extra features above is wrapper-qualified.
The ledger is an audit, not a replacement for paired native/module/facade tests.
