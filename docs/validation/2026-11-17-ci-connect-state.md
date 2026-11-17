# CI caller-state race — 17 November 2026

The initial push run for `cca021d` failed in the Linux Debug ASan/UBSan
`real_providers_reversed` test with `Caller setup failed`. Its sanitizer log
contains no ASan/UBSan diagnostic. The separate PR-triggered run passed, and a
rerun of the original push run also passed. Those successes do not erase the
original failure.

## Cause and correction

Haivision 1.5.7 `CUDTSocket::getStatus()` interprets `SRTS_CONNECTING` with both
`m_bConnecting == false` and `m_bConnected == false` as broken. Successful caller
completion cleared `m_bConnecting` before looking up the socket and publishing
`m_bConnected`. A concurrent state query could therefore misclassify an ordinary
successful handshake as a terminal failure. The adapter correctly propagated
that native state; the test correctly stopped on a terminal error.

The local compatibility patch now publishes connected before retiring connecting,
using the existing native connection lock. It adds no lock, delay or retry to the
production path and does not suppress genuine rejected or failed connections.
The change is documented in `providers/haivision-1.5.7-connect-state.patch` and
included in the patched-source hash and provider fingerprint. Both direct and
wrapped paths use the same corrected native library.

Connection-setup failures in the integration test now include provider names,
mode, encryption, IP family, profile setup selection and terminal error details.
No test timeout, acceptance condition or CI gate was relaxed.

## Reproduction and regression

- The unmodified candidate's connection tests reproduced `Caller setup failed`
  in both load orders in Ubuntu 24.04 ARM64 with GCC 13.3 and Debug ASan/UBSan.
- Widening the native flag transition window in a temporary diagnostic build
  reproduced the same error on macOS ARM64.
- The new deterministic `haivision_connect_state` test pauses the real caller
  handshake immediately after it clears `m_bConnecting`. Before correction it
  fails with `successful handshake transiently reported broken`. With the
  correction it observes a nonterminal state, resumes the handshake and verifies
  an actual payload transfer.
- The test uses a private copy of the patched native core linked into a separate
  executable, with the vendor's own layout-affecting flags and language mode.
  Production provider modules contain neither the test callback nor a pause.

Local corrected-candidate validation on macOS ARM64 passed all 16 Release CTest
checks (73.89 seconds) and all 16 RelWithDebInfo ASan/UBSan checks (78.45 seconds),
including real providers, encryption rejection, polling regressions, exports,
benchmark failure cleanup and the deterministic caller-state test. Whitespace
validation passed. The two distributed native patches exactly reproduce the
compiled production core when applied to the pinned upstream source.

These are local results; the original Linux failure and baseline reproductions
above remain part of the record. Fresh corrected-candidate CI must be checked
separately.

Performance qualification remains incomplete. This native source change produces
new module fingerprints, and previous benchmark captures do not qualify it.
