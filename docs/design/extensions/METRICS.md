# Native telemetry catalogue

Descriptions adapted from Haivision SRT 1.5.7 `srtcore/srt.h`, copyright (c) 2018
Haivision Systems Inc., under the [Mozilla Public License 2.0](https://mozilla.org/MPL/2.0/).

Names preserve native field semantics, not application-delivery accounting. All values
come from one non-clearing `srt_bstats(socket, ..., 0)` call per snapshot. Native
local/interval measurements are not wrapper intervals; queries never reset them.
Signed native counters remain signed (including native overflow behavior). A zero
is a native observation, not a guarantee that a feature is active. RTT/bandwidth
are estimates. Byte counters are native accounting, not IP wire goodput.

| Name | Type | Unit | Native meaning |
| --- | --- | --- | --- |
| `native.msTimeStamp` | int64 | milliseconds | time since the UDT entity is started, in milliseconds |
| `native.pktSentTotal` | int64 | packets | total number of sent data packets, including retransmissions |
| `native.pktRecvTotal` | int64 | packets | total number of received packets |
| `native.pktSndLossTotal` | int64 | packets | total number of lost packets (sender side) |
| `native.pktRcvLossTotal` | int64 | packets | total number of lost packets (receiver side) |
| `native.pktRetransTotal` | int64 | packets | total number of retransmitted packets |
| `native.pktSentACKTotal` | int64 | packets | total number of sent ACK packets |
| `native.pktRecvACKTotal` | int64 | packets | total number of received ACK packets |
| `native.pktSentNAKTotal` | int64 | packets | total number of sent NAK packets |
| `native.pktRecvNAKTotal` | int64 | packets | total number of received NAK packets |
| `native.usSndDurationTotal` | int64 | microseconds | total time duration when UDT is sending data (idle time exclusive) |
| `native.pktSndDropTotal` | int64 | packets | number of too-late-to-send dropped packets |
| `native.pktRcvDropTotal` | int64 | packets | number of too-late-to play missing packets |
| `native.pktRcvUndecryptTotal` | int64 | packets | number of undecrypted packets |
| `native.byteSentTotal` | uint64 | bytes | total number of sent data bytes, including retransmissions |
| `native.byteRecvTotal` | uint64 | bytes | total number of received bytes |
| `native.byteRcvLossTotal` | uint64 | bytes | total number of lost bytes |
| `native.byteRetransTotal` | uint64 | bytes | total number of retransmitted bytes |
| `native.byteSndDropTotal` | uint64 | bytes | number of too-late-to-send dropped bytes |
| `native.byteRcvDropTotal` | uint64 | bytes | number of too-late-to play missing bytes (estimate based on average packet size) |
| `native.byteRcvUndecryptTotal` | uint64 | bytes | number of undecrypted bytes |
| `native.pktSent` | int64 | packets | number of sent data packets, including retransmissions |
| `native.pktRecv` | int64 | packets | number of received packets |
| `native.pktSndLoss` | int64 | packets | number of lost packets (sender side) |
| `native.pktRcvLoss` | int64 | packets | number of lost packets (receiver side) |
| `native.pktRetrans` | int64 | packets | number of retransmitted packets |
| `native.pktRcvRetrans` | int64 | packets | number of retransmitted packets received |
| `native.pktSentACK` | int64 | packets | number of sent ACK packets |
| `native.pktRecvACK` | int64 | packets | number of received ACK packets |
| `native.pktSentNAK` | int64 | packets | number of sent NAK packets |
| `native.pktRecvNAK` | int64 | packets | number of received NAK packets |
| `native.mbpsSendRate` | double | megabits/second | sending rate in Mb/s |
| `native.mbpsRecvRate` | double | megabits/second | receiving rate in Mb/s |
| `native.usSndDuration` | int64 | microseconds | busy sending time (i.e., idle time exclusive) |
| `native.pktReorderDistance` | int64 | packets | size of order discrepancy in received sequences |
| `native.pktRcvAvgBelatedTime` | double | milliseconds | average time of packet delay for belated packets (packets with sequence past the ACK) |
| `native.pktRcvBelated` | int64 | packets | number of received AND IGNORED packets due to having come too late |
| `native.pktSndDrop` | int64 | packets | number of too-late-to-send dropped packets |
| `native.pktRcvDrop` | int64 | packets | number of too-late-to play missing packets |
| `native.pktRcvUndecrypt` | int64 | packets | number of undecrypted packets |
| `native.byteSent` | uint64 | bytes | number of sent data bytes, including retransmissions |
| `native.byteRecv` | uint64 | bytes | number of received bytes |
| `native.byteRcvLoss` | uint64 | bytes | number of retransmitted bytes |
| `native.byteRetrans` | uint64 | bytes | number of retransmitted bytes |
| `native.byteSndDrop` | uint64 | bytes | number of too-late-to-send dropped bytes |
| `native.byteRcvDrop` | uint64 | bytes | number of too-late-to play missing bytes (estimate based on average packet size) |
| `native.byteRcvUndecrypt` | uint64 | bytes | number of undecrypted bytes |
| `native.usPktSndPeriod` | double | microseconds | packet sending period, in microseconds |
| `native.pktFlowWindow` | int64 | packets | flow window size, in number of packets |
| `native.pktCongestionWindow` | int64 | packets | congestion window size, in number of packets |
| `native.pktFlightSize` | int64 | packets | number of packets on flight |
| `native.msRTT` | double | milliseconds | RTT, in milliseconds |
| `native.mbpsBandwidth` | double | megabits/second | estimated bandwidth, in Mb/s |
| `native.byteAvailSndBuf` | int64 | bytes | available UDT sender buffer size |
| `native.byteAvailRcvBuf` | int64 | bytes | available UDT receiver buffer size |
| `native.mbpsMaxBW` | double | megabits/second | Transmit Bandwidth ceiling (Mbps) |
| `native.byteMSS` | int64 | bytes | MTU |
| `native.pktSndBuf` | int64 | packets | UnACKed packets in UDT sender |
| `native.byteSndBuf` | int64 | bytes | UnACKed bytes in UDT sender |
| `native.msSndBuf` | int64 | milliseconds | UnACKed timespan (msec) of UDT sender |
| `native.msSndTsbPdDelay` | int64 | milliseconds | Timestamp-based Packet Delivery Delay |
| `native.pktRcvBuf` | int64 | packets | Undelivered packets in UDT receiver |
| `native.byteRcvBuf` | int64 | bytes | Undelivered bytes of UDT receiver |
| `native.msRcvBuf` | int64 | milliseconds | Undelivered timespan (msec) of UDT receiver |
| `native.msRcvTsbPdDelay` | int64 | milliseconds | Timestamp-based Packet Delivery Delay |
| `native.pktSndFilterExtraTotal` | int64 | packets | number of control packets supplied by packet filter |
| `native.pktRcvFilterExtraTotal` | int64 | packets | number of control packets received and not supplied back |
| `native.pktRcvFilterSupplyTotal` | int64 | packets | number of packets that the filter supplied extra (e.g. FEC rebuilt) |
| `native.pktRcvFilterLossTotal` | int64 | packets | number of packet loss not coverable by filter |
| `native.pktSndFilterExtra` | int64 | packets | number of control packets supplied by packet filter |
| `native.pktRcvFilterExtra` | int64 | packets | number of control packets received and not supplied back |
| `native.pktRcvFilterSupply` | int64 | packets | number of packets that the filter supplied extra (e.g. FEC rebuilt) |
| `native.pktRcvFilterLoss` | int64 | packets | number of packet loss not coverable by filter |
| `native.pktReorderTolerance` | int64 | packets | packet reorder tolerance value |
| `native.pktSentUniqueTotal` | int64 | packets | total number of data packets sent by the application |
| `native.pktRecvUniqueTotal` | int64 | packets | total number of packets to be received by the application |
| `native.byteSentUniqueTotal` | uint64 | bytes | total number of data bytes, sent by the application |
| `native.byteRecvUniqueTotal` | uint64 | bytes | total number of data bytes to be received by the application |
| `native.pktSentUnique` | int64 | packets | number of data packets sent by the application |
| `native.pktRecvUnique` | int64 | packets | number of packets to be received by the application |
| `native.byteSentUnique` | uint64 | bytes | number of data bytes, sent by the application |
| `native.byteRecvUnique` | uint64 | bytes | number of data bytes to be received by the application |
