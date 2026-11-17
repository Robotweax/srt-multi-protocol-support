# Multi-provider srt-live-transmit bundle

Run `bin/srt-live-transmit -h` for supported options. Both providers, when
included, are found automatically in `lib/srtmux/providers` relative to the
executable. The default provider is Haivision; choose `--provider robotweax` or
use `provider=haivision` / `provider=robotweax` on each SRT URI.

```sh
bin/srt-live-transmit --provider robotweax \
  'udp://:5000' 'srt://receiver.example:9000?mode=caller'
```

The binary and provider modules must match your operating system and CPU.
The provider modules require the system OpenSSL runtime used by their build;
OpenSSL is not bundled. Media uses stdout only for an explicit console target;
diagnostics use stderr.

See [application documentation](share/srtmux/README.md) for the supported CLI,
encryption, reconnects, EOF behavior and limitations. This is a Live relay:
missing trailing Live packets are allowed. It is not a reliable File-transfer
utility or a throughput/WAN qualification claim.

License notices, source provenance and the native compatibility patches are
under `share/srtmux`. Public source and source pins are available in the
[project repository](https://github.com/Robotweax/srt-multi-protocol-support).
`SHA256SUMS`, when supplied with the bundle, records all packaged files.
