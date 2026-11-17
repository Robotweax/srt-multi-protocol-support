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

`SRTMUX_BUILD_APPLICATIONS` defaults on for POSIX platforms. It can also build
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
boundaries; oversized input datagrams fail rather than being truncated. Set
`-c` to the required size, at most 1,456 bytes when SRT is involved.

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
| Profile tuning | `maxbw`, `inputbw`, `oheadbw`, `fc`, `ipttl`, `iptos`, `ipv6only`, `kmrefreshrate`, `kmpreannounce`, `udpsndbuf`, `udprcvbuf` |
| Required Live policies | `transtype=live`, enabled `tsbpd`/`tsbpdmode`, `messageapi`, `enforcedencryption` |
| UDP | IPv4/IPv6 unicast; IPv4 multicast; `adapter` as numeric IPv4, `ttl`, `tos`, `buffer_size`, `multicast` |
| Relay | `-c` chunk, `-buffering` bounded packet queue, `-a` reconnect (default yes) |
| Timeout | `-t` seconds, `-tm:0` whole-run / `-tm:1` disconnected timer |
| Diagnostics | `-q`, `-v`, `-s`/`-r` packet-frequency reports; `-statsout`, `-pf:default/json` |

Statistics include application bytes/packets and average application bitrate.
JSON also includes non-clearing native packet, retransmission, drop and RTT
snapshots for connected SRT endpoints. This is a documented subset of upstream's
statistics formatting, not its entire console/CSV reporting interface.
Verbose diagnostics identify the actual module release and build fingerprint.
They do not print complete URIs, passphrases or stream IDs.

Native log controls/callbacks, full-statistics/CSV output, source-time forwarding,
rendezvous, TCP/RTP/HTTP/RTMP, IPv6 multicast, groups, FEC/GCM and reliable SRT
File mode are rejected. The facade's source clocks are provider-specific;
the app does not forward a timestamp into a different clock domain.

## End of stream and reconnects

A missing tail at the end of a Live transmission is allowed. File endpoints
feed or capture a **Live stream** and do not grant it reliable File semantics.
The relay does not implement the separate `srt-file-transmit` application.

On local input EOF, the app sends its queued messages then keeps the target
alive for `--eof-grace-ms` (default 1,000 ms). This is bounded best-effort grace,
not a guaranteed native drain-close. Ctrl-C, SIGTERM and the configured timer
stop promptly, close their owned endpoints and return success; already queued
data may remain unsent. I/O and setup errors return failure. The 250-ms reconnect
retry delay does not bypass the configured native connection timeout. A single
listener serves one active peer at a time and accepts a new peer after a
disconnect. With `-a:no`, an established session's disconnect stops normally;
failure to establish the initial connection still returns failure.

The relay bounds its user-space queue by `-buffering` (default 10 packets) and
applies backpressure to its input when full. UDP senders may still lose datagrams
in OS/network queues under overload. Throughput/WAN qualification of this new
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
