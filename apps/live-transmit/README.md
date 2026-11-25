# Multi-provider srt-live-transmit

This port preserves Haivision SRT 1.5.7's `srt-live-transmit [options] input output`
workflow and supported CLI options. The application uses `srtmux::Provider`,
profiles, connections and pollers for every SRT operation. It contains no native
SRT/UDT calls and links neither vendor library directly. The adapted Haivision
URI parser retains its [MPL-2.0 license and provenance](haivision/README.md).

The initial OS I/O backend supports Linux and macOS. The provider interface and
host library remain separate from that application platform scope.

## Build and run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSRTMUX_BUILD_PROVIDERS=ON
cmake --build build --parallel
build/apps/srt-live-transmit -h
```

`SRTMUX_BUILD_APPLICATIONS` defaults on for POSIX platforms. For Windows x64, use the portable ZIP, or enable the application explicitly with `-DSRTMUX_BUILD_APPLICATIONS=ON` in an x64 MSVC environment. The ZIP includes both provider DLLs and their non-system dependencies.

The relay can also build
without `SRTMUX_BUILD_PROVIDERS`; UDP/file workflows then need no provider or
vendor download. For SRT, the default provider is Haivision. Modules are loaded
from the build's `providers` directory or the installed `lib/srtmux/providers`
directory relative to the executable. `--provider-dir` and explicit
`--haivision-module` / `--robotweax-module` paths override that location. No
module search uses the working directory. Modules are trusted native code.

Receive UDP and transmit using Robotweax:

```sh
build/apps/srt-live-transmit --provider robotweax \
  'udp://:5000' 'srt://receiver.example:9000?mode=caller'
```

Receive using Haivision and forward using Robotweax in the same process:

```sh
build/apps/srt-live-transmit \
  'srt://:9000?mode=listener&provider=haivision' \
  'srt://receiver.example:9001?mode=caller&provider=robotweax'
```

Receive either peer's SRT stream and write UDP:

```sh
build/apps/srt-live-transmit --provider robotweax \
  'srt://:9000?mode=listener' 'udp://127.0.0.1:5000'
```

Console and regular-file sources/sinks are supported (`file://con` means
stdin/stdout). Media bytes use stdout only when requested; diagnostics and
default statistics always use stderr. Files use `file:///absolute/path` or a
plain path. Quote URIs containing `&` in the shell. UDP preserves datagram
boundaries. An input datagram larger than `-c` is dropped whole and counted as
lost; it is never truncated and never stops the relay. As in the original, `-c`
is the read size (default 1,456) and the SRT `payloadsize` option is the message
limit (default 1,316, at most 1,456); they are independent. A packet larger than
the SRT target's payload size is counted as lost, so raise `payloadsize` to carry
larger datagrams. For an SRT input `-c` must be at least its `payloadsize`.

## CLI compatibility and supported settings

Options accept Haivision's `-name:value`, `-name value` and the port's
`--name=value` forms. `-h` lists supported names and aliases. Unknown command
options, URI options and unsupported values fail explicitly.

| Setting | Support |
| --- | --- |
| Source/target | SRT Live, UDP, console, regular files |
| SRT role | Caller/listener, numeric IPv4/IPv6 and hostnames resolved by the app |
| Provider | `--provider` default; URI `provider=haivision` or `robotweax` per endpoint |
| SRT security | `passphrase`, `pbkeylen=16/24/32`; required AES-CTR when configured |
| SRT timing | `latency`, `rcvlatency`, `peerlatency`, `conntimeo`, `peeridletimeo` |
| SRT socket setup | `adapter`, `bind`, `port`, `mss`, `payloadsize`, `sndbuf`, `rcvbuf`, `streamid` |
| Robotweax only | `npd=required` / `npd=if_offered`: MPEG-TS null packet deletion (experimental) |
| Profile tuning | `maxbw`, `inputbw`, `oheadbw`, `fc`, `ipttl`, `iptos`, `ipv6only`, `kmrefreshrate`, `kmpreannounce`, `udpsndbuf`, `udprcvbuf` |
| Required Live policies | `transtype=live`, enabled `tsbpd`/`tsbpdmode`, `messageapi`, `enforcedencryption` |
| UDP | IPv4/IPv6 unicast; IPv4 multicast; `adapter` as numeric IPv4, `ttl`, `tos`, `buffer_size`, `multicast` |
| Relay | `-c` chunk (default 1456), `-buffering` packets read per pass (default 10), `-a` reconnect (default yes) |
| Timeout | `-t` seconds, `-tm:0` whole-run / `-tm:1` disconnected timer |
| Port additions | `--coalesce-ms` input batching (default off), `--eof-grace-ms`, provider selection |
| Diagnostics | `-q`, `-v`, `-s`/`-r` packet-frequency reports; `-statsout`, `-pf:default/json/csv` (also `json,pretty`), `-f` |
| Accepted, not applied | `-ll`, `-lfa`, `-logfile`, `-loginternal` |

Statistics follow the original: each connected SRT endpoint reports every `-s`
packets of its own, in the original's two-column (`default`), `json` or `csv`
layout with the same fields, names and order, and `-r` prints the bandwidth
line. Reports cover the interval since the previous report, or running totals
with `-f`. The provider interface never clears native counters, so the interval
values and rates are computed by the app from successive snapshots. `sid` is a
per-connection number, because native socket IDs are not exposed. Statistics go
to stderr unless `-statsout` names a file, since stdout may carry media.

Two extra formats report the relay's own counters with provider names:
`-pf:relay` and `-pf:relay-json` (bytes read, written and lost, packets, average
bitrate, and a short native snapshot per SRT endpoint).

The logging options are accepted so existing command lines keep working, but
they are not applied: the provider interface has no native log control. A note
on stderr says so.
Verbose diagnostics identify the actual module release and build fingerprint.
They do not print complete URIs, passphrases or stream IDs.

Source-time forwarding,
rendezvous, TCP/RTP/HTTP/RTMP, IPv6 multicast, groups, FEC/GCM and reliable SRT
File mode are rejected. The facade's source clocks are provider-specific;
the app does not forward a timestamp into a different clock domain.

## Null packet deletion

`npd` is an addition to the original's options and works on Robotweax endpoints
only. It removes MPEG-TS null packets on the wire and restores them at the
receiver, so a constant-bitrate stream does not spend bandwidth on stuffing; the
relayed bytes are unchanged. Robotweax SRT classes it as experimental.

```sh
# Receiver: use it when the caller offers it, serve other callers normally.
srt-live-transmit 'srt://:9000?mode=listener&provider=robotweax&npd=if_offered' 'udp://127.0.0.1:5000'
# Sender: require it.
srt-live-transmit 'udp://:5001' 'srt://receiver:9000?provider=robotweax&npd=required'
```

- `npd=required` fails the connection unless the peer negotiates it, so a
  Robotweax caller with it cannot reach a Haivision or original listener.
- `npd=if_offered` is a listener setting. A caller without the feature,
  including the original application, is served with ordinary SRT.
- On a Haivision endpoint any `npd` value other than `off` is an error.
- Only 188-byte-aligned payloads of up to seven packets are affected, which is
  what `-c:1316` UDP transport streams carry. Other payloads pass unchanged.
- With `-v` the app reports whether it was negotiated, and `-pf:relay-json`
  adds the `robotweax.npd.*` counters for that endpoint.

## End of stream and reconnects

A missing tail at the end of a Live transmission is allowed. File endpoints
feed or capture a **Live stream** and do not grant it reliable File semantics.
The relay does not implement the separate `srt-file-transmit` application.

On local input EOF, the app keeps the target alive for `--eof-grace-ms` (default
1,000 ms). This is bounded best-effort grace, not a guaranteed native
drain-close. Ctrl-C, SIGTERM and the configured timer stop promptly, close their
owned endpoints and return success. I/O and setup errors return failure.

Connection handling follows the original:

- A listener serves one client at a time. It stops listening while that client
  is connected, so further callers are refused rather than connected and
  ignored, and it listens again after the client leaves.
- With reconnect enabled (the default) a failed or lost connection is retried
  every 250 ms. A failed attempt is reported once per distinct reason, for
  example `SRT target connection failed: wrong or missing passphrase; retrying`.
  The retry delay does not bypass the configured native connection timeout.
- With `-a:no`, an established session's disconnect stops normally; failure to
  establish the initial connection returns failure and names the reason.

Status lines match the original where the event is the same: `Media path: ...`
(without the URI query, which can hold a passphrase), `SRT source connected`,
`SRT target connected`, `Accepted SRT source connection`, `SRT target
disconnected` and the periodic `N bytes lost, M bytes sent, K bytes received`.

## Data handling under load

The relay follows the original's Live rule: it reads up to `-buffering` packets
per pass and hands each one to the target at once. A packet the target cannot
take, because the target is not connected, its buffer is full, or the packet
exceeds its message limit, is counted as lost and reported every five seconds
while losses continue. Live data is not held back and delivered late.

File and console sources are the one exception, because pausing them loses
nothing: they are not read while the target is unavailable or full, and they are
read in units the SRT target can carry.

While both endpoints are connected the relay waits in the source's poller and
wakes only for data or a state change.

### CPU cost and `--coalesce-ms`

By default the relay forwards each packet as soon as it arrives. That keeps
delay and output timing tight, but one wake-up per packet costs CPU: in local
measurements this relay used about 1.4 to 1.9 times the CPU of the original
application at 5 and 20 Mbit/s. The original checks UDP input on the library's
10 ms tick and so handles packets in batches.

`--coalesce-ms <n>` (0 to 100, default 0) makes this relay batch as well: after
emptying its source it stays away for `n` milliseconds. With `--coalesce-ms 10`
its CPU use was at or below the original's in the same measurements, with every
packet delivered. The price is up to `n` milliseconds of added delay and
burstier output, so leave it off where timing matters more than CPU. See the
[measurements](../../docs/validation/2026-11-20-relay-cpu.md). Throughput/WAN qualification of this
application is separate from functional interoperability and remains pending.

## Install a bundle

```sh
cmake --install build --prefix "$PWD/build/dist" --component SrtMux
build/dist/bin/srt-live-transmit -version
```

The component includes the application, both provider modules when built, and
their license notices. OpenSSL remains a system runtime dependency of the
providers. The provider pins, Haivision compatibility patches and existing
qualification limits are described in [providers/README.md](https://github.com/Robotweax/srt-multi-protocol-support/blob/main/providers/README.md).
