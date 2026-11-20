# Null packet deletion through the provider interface — 20 November 2026

Robotweax SRT 0.3.4 includes experimental MPEG-TS null packet deletion (NPD).
This change makes it reachable through the profile extension, the C++ facade and
the relay application, on Robotweax only. Nothing changes for a profile or URI
that does not ask for it, and Haivision rejects it explicitly.

## What was added

- `srtmx_profile_v1.null_packet_deletion` replaces a reserved, must-be-zero word;
  the record layout is unchanged. C++: `ProfileConfig::null_packet_deletion`.
- Four Robotweax-only metrics after the common catalogue: `robotweax.npd.active`,
  `.deleted_units`, `.restored_units`, `.malformed_payloads`, read from the
  vendor's cumulative getter. The 82 common metrics and their IDs are unchanged.
- Relay application: `npd=required|if_offered` on an SRT URI, a verbose line
  reporting the negotiated result, and the counters in `-pf:relay-json`.

The adapter enables this when the vendor header declares the feature, so a
module built against an older Robotweax release reports it as unavailable.

## Limits inherited from the vendor

Experimental status and provisional identifiers; individual Live caller/listener
connections only; 188-byte packets and canonical null packets only; one byte less
maximum Live payload when a non-OFF policy is set; no negotiation with Haivision,
where a REQUIRED caller fails setup. See the vendor's NPD contract for the
qualification still outstanding. This layer adds no claim beyond it.

## Checks

macOS ARM64 Release, both real providers:

- Provider tests: a Robotweax pair with REQUIRED on both sides, and with a
  REQUIRED caller and an IF_OFFERED listener, delivers 20 messages bit-exactly,
  including one of seven null packets and one that is not a transport stream;
  the sender reports 61 deleted and the receiver 61 restored null packets, with
  no malformed payloads. A Haivision caller is served by an IF_OFFERED
  Robotweax listener with NPD inactive. Haivision profile creation, a File
  profile, an unknown policy value and a caller with IF_OFFERED all fail
  explicitly.
- Application tests: a transport stream relayed UDP to SRT to UDP arrives
  unchanged with NPD negotiated between Robotweax endpoints and with a Haivision
  caller where it is not; `npd=required` on a Haivision endpoint is an error.
- The full suite of 22 tests passes, the unmodified Haivision 1.5.7 application
  still exchanges 24 of 24 messages with both providers, and the native surface
  inventory still verifies against both pinned source trees.

## Wire saving on a sample stream

One local run through the relay application, Robotweax to Robotweax over
loopback, 400 messages of seven transport packets of which three are null
packets (43% stuffing). All 400 messages arrived unchanged in both runs.

| NPD | Payload bytes sent | Bytes on the wire |
| --- | ---: | ---: |
| off | 525,084 | 542,640 |
| negotiated | 525,084 | 318,003 |

That is 41% fewer wire bytes for this synthetic stream. The saving depends
entirely on how much stuffing a stream carries; this is not a measurement of a
real encoder output or of a physical network.

