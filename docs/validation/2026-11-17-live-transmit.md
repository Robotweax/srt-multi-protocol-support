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
  and rejection of oversized UDP input without silently truncating its payload.

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
