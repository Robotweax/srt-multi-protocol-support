# Polling review fixes — 17 November 2026

This change addresses two defects found while reviewing PR #1 at
`2a90282cad9ba32ef3573dc88c1175d5a9531815`.

## Ready-event starvation

Previously, any pending locally closed registration made `poller_wait` return
only the terminal-event prefix before consulting native readiness. Readable live
connections were hidden even with spare output capacity; later closed objects
were hidden indefinitely when the terminal list exceeded that capacity.

Pending terminal readiness now makes the native wait nonblocking. The bounded
merge alternates priority between terminal and native events and rotates through
terminal registrations, including with a one-slot output. Closed events remain
level-triggered, and user cancellation still takes priority. Polling continues to
use fixed scratch space and visits at most one output batch of terminal entries.

## Facade registration lifetime

Previously, native object release removed native subscriptions but left entries
in the C++ poller's map. A long-lived poller retained one entry per historical
registration, and each addition scanned every historical entry.

The facade now tracks non-owning links in both directions, protected by the
provider context's cold-path registration mutex. Destroying either object or
poller removes the reverse links. Explicit removal and failed additions also
clean up both directions. Wrapper moves preserve the stable implementation
addresses. Duplicate detection is an object-local hash lookup; no registration
map access or additional lock was added to send, receive or wait.

A token retired by object destruction is unknown to facade update/remove, just
like a token explicitly removed. Already returned events remain advisory and may
contain stale tokens, as before. Registration does not retain an object.

## Regression evidence

The new mixed-readiness test failed on the original implementation in both
provider load orders. With only the readiness fix applied, those checks passed
and the new lifetime check failed because a destroyed object's token remained
registered. Both checks then passed with the lifetime fix.

The tests cover mixed native/terminal readiness with spare capacity, one-slot
fairness across three closed objects and a readable connection, infinite waits,
wake priority, repeated connection destruction across two pollers, duplicate
rejection, wrapper moves, failed-add rollback, removal racing with object
destruction, listener destruction, and poller destruction before its object.

Final local validation on macOS ARM64:

- Release, real providers and benchmarks enabled: 15/15 CTest checks passed
  (75.08 seconds).
- RelWithDebInfo with ASan/UBSan, real providers and benchmarks enabled:
  15/15 CTest checks passed (79.69 seconds).
- Release and sanitizer builds used identical changed runtime and test sources.
- Whitespace validation (`git diff --check`) passed.

These are local results, not a claim that GitHub CI or other platforms passed.

## Qualification boundary

No ABI or wire format changes were made. This changes provider module source and
build fingerprints; earlier performance captures do not qualify these new bytes.
The benchmark smoke tests check functionality only. No full throughput or CPU
qualification campaign was rerun, and PR #1 must remain in draft with its existing
performance blocker open.
