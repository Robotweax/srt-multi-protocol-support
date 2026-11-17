# Multi-provider application validation — 17 November 2026

The new `srt-live-transmit` ports Haivision's supported source/target CLI workflow
to the multi-SRT facade. The adapted MPL-2.0 URI parser is attributed to the
pinned Haivision 1.5.7 source. Native SRT/UDT is neither called nor linked into
the executable; each SRT endpoint selects its own runtime provider module.

## Functional coverage

- Both providers in all four sender/receiver combinations, plaintext and AES-128.
- Global provider selection and per-URI overrides, with verbose confirmation of
  the actual provider identity/release/fingerprint.
- Two SRT-to-SRT bridges loading different libraries in the same process, in both
  directions, with UDP feed/capture outside the bridge.
- IPv6 UDP (including empty datagrams) and a cross-provider IPv6 SRT path on
  IPv6-capable hosts; CLI/files remain usable without any provider modules.
- Reconnection of both caller and listener roles after the peer restarts, including
  replacing a peer with the other library.
- Wrong encryption keys and explicit wrong-module identities fail.
- Finite local-file input, Live SRT transport, file capture, normal peer closure,
  and an incomplete final message; the captured payload must be an intact prefix.
- Native non-clearing statistics, quiet/verbose output, no URI passphrase echo,
  malformed/duplicate options, numeric bounds, percent encoding and unknown features.
- Binary stdin/stdout preservation, local file alias protection, timer/signal exit,
  and oversized UDP input being dropped whole, without truncation and without
  stopping the relay.

Finite Live captures require a nonempty intact prefix. Missing trailing data are
allowed by the accepted Live rule; inner gaps, reordering or byte changes fail
the external payload comparisons. This does not change the existing reliable
File checks or turn functional tests into performance qualification samples.

## Independent upstream interoperability

An independently built, unmodified Haivision `srt-live-transmit` from revision
`899348d8318eb9a3c5a5b6ec43c4a1114288773a` (Release, `USE_ENCLIB=openssl`) is the
black-box peer. Both new-app providers send to and receive from that executable,
with and without AES-128: eight cases. Every local case delivered all 24 expected
messages with no missing tail. Both process exits are checked.

The reproducible test entry point is `tests/live_transmit_reference_test.py`,
which accepts the new executable, upstream executable and both module paths.
Release CI builds this reference separately from the modules and runs the same
eight cases. The reference is not the multi-provider executable and is not bundled.

## Local checks and packaging

The application CLI and provider checks pass on macOS ARM64 Release and Linux
ARM64 Debug ASan/UBSan. Symbol inspection additionally checks the executable has
no native SRT/UDT entry points. Existing transport/provider production sources,
native pins and the previously published qualification data are unchanged.

The `SrtMux` install component stages the executable, both built provider modules,
MIT/MPL notices and the native compatibility patches. The installed binary is
tested from a different working directory containing invalid lookalike modules;
it still resolves each real provider relative to its installation. Bundles include
source/build information and a complete SHA-256 manifest. CI archives use tar to
preserve executable permissions and publish separate Linux/macOS artifacts.

The application currently supports Linux/macOS. Unsupported native options fail
explicitly; see the [CLI scope and examples](../../apps/live-transmit/README.md).
Application CPU/throughput, WAN behavior and broader deployment qualification
remain separate work. The earlier foundation performance assessment is retained.

## Review fixes — 17 November 2026

A review of the first revision of this application found four defects, each
reproduced with the built executable before the fix and covered by a test now:

| Defect | Before | After |
| --- | --- | --- |
| Idle connected SRT source | 5.01 s (Haivision) and 1.54 s (Robotweax) process CPU in 5 s | 0.03 s and 0.00 s |
| Wrong passphrase with reconnect enabled | Retried four times a second, 30 identical lines in 8 s, no reason given | Retried, reason reported once |
| Second caller to a busy listener | Reported connected and sent into nothing | Refused until the listener is free |
| Oversized UDP datagram | Stopped the relay with exit status 1 | Dropped, counted as lost, relay continues |

The idle spin came from keeping the level-triggered state interest registered
after connecting and reading connection state on every pass. State is now read
when the poller reports a change, with a 100 ms check for the endpoint the relay
is not waiting on.

The relay was also brought closer to the original: `-c` defaults to 1,456 and is
independent of `payloadsize`, data the target cannot take is counted as lost
instead of being queued and delivered late, and status lines use the original's
wording. The unmodified Haivision 1.5.7 application still exchanges all 24
messages with both providers, in both directions, plaintext and AES-128.
The accepted Live tail rule and its checks are unchanged.

## Statistics and logging options — 17 November 2026

`-s`, `-r`, `-pf:default|json|csv` (and `json,pretty`), `-f` and `-statsout` now
behave as in the original for SRT endpoints. The interoperability check runs the
unmodified Haivision 1.5.7 application and this port with `-s:4 -pf:json` in all
eight cases and requires the same JSON objects, keys and key order from both.
Provider tests check the CSV heading, the two-column layout, the bandwidth line,
per-report intervals and `-f` running totals for both providers.

Native counters are never cleared through the provider interface, so interval
values and rates are computed by the app from successive snapshots, and `sid` is
a per-connection number rather than a native socket ID. The relay's own counters
remain available as `-pf:relay` and `-pf:relay-json`.

`-ll`, `-lfa`, `-logfile` and `-loginternal` are accepted and reported as not
applied, so existing command lines run unchanged. They cannot take effect until
the provider interface offers native log control.
