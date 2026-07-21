# mwx Hang Debugging

## Symptom

The `mwx(4)` interface occasionally stops passing packets while still
appearing up and associated:

```text
mwx0: flags=a48847<UP,BROADCAST,DEBUG,RUNNING,SIMPLEX,MULTICAST,...>
        media: IEEE802.11 HT-MCS0 mode 11n (HT-MCS7 mode 11n)
        status: active
        ieee80211: nwid EE-C75NQ7 chan 100
            bssid b8:6a:f1:bf:de:71 -51dBm ...
```

Traffic to the local gateway fails even when explicitly bound to the
wireless address:

```text
$ ping -c 3 -I 192.168.1.68 192.168.1.254
PING 192.168.1.254 (192.168.1.254): 56 data bytes

--- 192.168.1.254 ping statistics ---
3 packets transmitted, 0 packets received, 100.0% packet loss
```

Taking the interface down and up normally recovers it. Ethernet can be used
as the preferred default route while preserving the broken wireless state
for inspection.

## Findings From a Live Failure

The failure was below IP. The gateway's ARP entry remained incomplete:

```text
192.168.1.254  (incomplete)  mwx0
```

Forced ARP and ping traffic increased the outbound interface counters, but
the input packet count remained fixed at `451000`. This rules out routing,
DNS, and an Internet-side failure.

The driver continued to receive TX completions and firmware notifications.
Most TX statuses initially claimed success, although later status included
`411` failed transmissions and `410` ACK errors. The firmware eventually
reported beacon loss with reason 6.

An earlier DEBUG-enabled stop dump captured the most important distinction:

```text
mwx0: activity age: intr 16 tx-intr 16 rx-intr 16 rx-packet 7438
    mcu-event 16 tx-free 16
```

Interrupts, RX-ring interrupts, MCU events, and TX-free events were all
recent, but the last actual radio packet was more than two hours old. The
same RX DMA path carries TX-status and firmware traffic, so an increasing
RX interrupt count does not prove that frames are still arriving over the
air.

At beacon loss the host-facing state still looked healthy:

```text
mwx0: ring data-tx (cons/prod/cpu/dma): 194/194/194/194
mwx0: ring data-rx (cons/prod/cpu/dma): 98/97/97/98
mwx0: progress: submitted 191 completed 191 ...
```

The TX ring was empty and synchronized, RX buffers remained posted, and no
TXWI was stuck. The 15-second TX watchdog therefore cannot detect this
failure because firmware continues completing host TX requests.

`ifconfig mwx0 scan` displayed the cached node table. It did not cause new
scan messages in `dmesg` and should not be treated as proof that live radio
reception has resumed. The displayed RSSI can likewise be stale.

The initial evidence pointed toward firmware/radio association state or
radio RX becoming wedged while the PCI DMA and host-facing firmware path
remained operational. It did not look like a generic PCI interrupt failure
or a full TX-ring lockup.

## Channel-Switch Finding

A second instrumented failure was captured on July 21, 2026. Ten pings
bound to `192.168.1.68` all failed. The interface counters changed as
follows:

```text
Ipkts 3010 -> 3010
Opkts 2675 -> 2685
Ofail  125 -> 135
```

A simultaneous 15-second radiotap capture contained nine outbound ICMP
echo requests and no inbound frames. The DEBUG-enabled stop dump showed:

```text
mwx0: progress: submitted 2689 completed 2689 ...
mwx0: tx status: txs 2689 retries 1885 failed 134 ack-errors 134
mwx0: rx types: normal 3009 normal-mcu 1271 txs 1486 ...
mwx0: radio rx: mgt 1271 beacon 1255 ... data 3009 hwdec 3008
mwx0: activity age: intr 44 tx-intr 44 rx-intr 44 rx-packet 874
    mcu-event 44 tx-free 44
mwx0: last radio rx: fc0 80 chan 100 rssi -54 flags 00000000
```

The host-facing firmware path was still active and all TX requests had
completed, but no over-the-air frame had arrived for more than 14 minutes.
The last frame was a beacon on channel 100.

The important new evidence came from recovery. Before teardown, the
interface and last received frame were on channel 100. Immediately after
firmware reload, the same AP BSSID (`b8:6a:f1:bf:de:71`) was found and
rejoined on channel 104, and gateway traffic worked again. This is strong
evidence that the AP changed channels while `mwx` remained tuned to the old
channel. It explains the apparent total radio-RX freeze without requiring
an RX DMA or radio hardware wedge.

Net80211 already recognized CSA and Extended CSA elements while parsing
beacons, but used them only to mark scan-table nodes as temporarily
unsuitable for association. It did not process CSA from the associated AP
in RUN state. By comparison, Linux mac80211 tracks the CSA target and
countdown and invokes the MT7921 channel-switch callbacks. The Linux driver
programs the new channel context at countdown expiry while retaining the
association.

The firmware's reason-6 beacon-loss event was not a reliable substitute for
CSA. It occurred shortly after association while normal traffic and beacons
were still arriving. The directed probe timer was correctly cleared by a
later beacon, but firmware did not send another useful event when the AP
eventually left channel 100.

## Existing Recovery and Timeout Paths

The driver has a 15-second watchdog based on the oldest outstanding TXWI.
It detects a complete firmware stall or an individual lost TX completion,
dumps status, and schedules a firmware reset.

MCU commands have a three-second timeout. A timeout also dumps status and
schedules a reset.

Neither path detects the observed failure because TX-free, TX-status, and
MCU traffic continue to flow.

The firmware beacon-loss event sends a directed probe request. Net80211 sets
`ic_mgt_timer` for this request and should move from RUN to SCAN if no beacon
or probe response clears the timer. During the observed failure the
interface remained in RUN, so the management timer and the exact type of
radio frames received after beacon loss need to be recorded.

An explicit down with `IFF_DEBUG` set calls `mwx_dump_status()` before
teardown. This is the safest existing way to capture live state immediately
before recovery.

## Added Instrumentation

`sys/dev/pci/if_mwx.c` now collects per-association RX statistics. The
statistics are reset when the interface enters RUN alongside the existing
TX statistics.

The following RX-ring packet classes are counted:

- Normal over-the-air packets.
- Normal-MCU packets.
- TX-status packets.
- Unknown packet types.
- Normal packets rejected by `mt7921_mac_fill_rx()`.

Successfully parsed 802.11 frames are classified as:

- Management frames.
- Beacons.
- Probe responses.
- Control frames.
- Data frames.
- Hardware-decrypted frames.

The driver also records the last accepted radio frame's frame-control byte,
channel, RSSI, RX flags, and arrival time.

Status dumps now include:

```text
mwx0: ... state ... mgt ... bmiss ... txwi ... timer ... wait ...
mwx0: tx status: txs ... retries ... failed ... ack-errors ...
mwx0: rx types: normal ... normal-mcu ... txs ... unknown ...
    parse-errors ...
mwx0: radio rx: mgt ... beacon ... probe-resp ... ctl ... data ...
    hwdec ...
mwx0: activity age: ... rx-packet ...
mwx0: last radio rx: fc0 ... chan ... rssi ... flags ...
```

These counters are printed only by the existing status-dump paths, such as
beacon loss, a timeout, or a DEBUG-enabled stop. They do not change packet,
association, timeout, or reset behavior.

## Capture Procedure

Build and boot a kernel containing the instrumentation. Ensure DEBUG is
enabled before waiting for another failure:

```sh
doas ifconfig mwx0 debug
```

When the interface hangs, do not take it down immediately. First save the
visible state and force a small amount of traffic:

```sh
stamp=$(date +%Y%m%d-%H%M%S)

ifconfig mwx0 > /tmp/mwx-ifconfig.$stamp
netstat -I mwx0 > /tmp/mwx-netstat-before.$stamp
arp -an > /tmp/mwx-arp.$stamp

ping -c 5 -I 192.168.1.68 192.168.1.254
sleep 2
netstat -I mwx0 > /tmp/mwx-netstat-after.$stamp
```

An optional radiotap capture may show whether beacons, probe responses, or
data frames are reaching the driver:

```sh
doas timeout 15 tcpdump -ni mwx0 -y IEEE802_11_RADIO \
    -s 512 -w /tmp/mwx-radio.$stamp.pcap &
capture_pid=$!
sleep 1
ping -c 5 -I 192.168.1.68 192.168.1.254
wait $capture_pid
```

Finally, take the interface down to trigger the pre-teardown status dump,
save `dmesg`, and bring the interface back:

```sh
doas ifconfig mwx0 down
dmesg > /tmp/mwx-dmesg.$stamp
doas ifconfig mwx0 up
```

The relevant section starts at the most recent `mwx0: stopping:` line. A
firmware beacon-loss dump shortly before it may also be useful.

## Interpreting the Next Dump

If `rx types: txs` increases while `normal` and `normal-mcu` remain fixed,
the RX DMA and firmware notification path is alive but over-the-air RX is
dead.

If management and beacon counts increase while data remains fixed, the
radio can still receive the AP but encrypted data delivery may be broken.
Compare the `hwdec` count and RX parse errors to investigate key,
decryption, or descriptor state.

If `parse-errors` increases, normal RX descriptors are reaching the driver
but `mt7921_mac_fill_rx()` is rejecting them. The next step would be to add
per-reason counters for band-index, A-MSDU, header-translation, ICV, FCS,
TKIP MIC, length, and descriptor-group failures.

If ACK errors and firmware-reported TX failures increase sharply, the AP is
not acknowledging transmissions even though the firmware is completing the
host requests.

If beacons or probe responses continue and `mgt` returns to zero, net80211
is correctly cancelling the missed-beacon probe timeout. The hang is then
more likely specific to data or crypto state than complete radio RX loss.

If no beacon or probe response is counted but `mgt` remains non-zero, inspect
whether `if_timer` continues invoking `mwx_watchdog()` and
`ieee80211_watchdog()`.

If no response is counted and `mgt` reaches zero without a RUN-to-SCAN
transition, inspect task scheduling and the driver's asynchronous
`mwx_newstate()` path.

For frame-control values in the last-radio-frame line, common values include:

```text
0x50  probe response
0x80  beacon
0x08  data
0x88  QoS data
```

An RX flags value containing `IEEE80211_RXI_HWDEC` indicates that firmware
reported successful hardware decryption.

## CSA Implementation

Net80211 now passes validated beacon CSA announcements from the associated
AP to a driver callback. The parser accepts CSA and Extended CSA elements,
checks the switch mode and target against the active channel set, and
rejects cross-band switches. If a driver has no channel-switch callback, or
the target is unsupported, net80211 falls back to scanning instead of
remaining on the abandoned channel.

The `mwx` callback:

- Tracks updates to the AP's CSA countdown.
- Honors CSA mode 1 by blocking new data transmissions until the switch.
- Uses the AP beacon interval and countdown to schedule the switch.
- Updates `ic_bss->ni_chan` and sends `MCU_EXT_CMD_CHANNEL_SWITCH` from the
  driver's serialized task queue.
- Keeps the existing association, keys, and BA state intact.
- Cancels stale CSA timers and tasks if the announcement disappears or the
  interface leaves RUN.
- Resets the device if firmware rejects or times out the channel-switch
  command.

The driver's current advertised PHY support is HT20, so the CSA target
channel is sufficient for its negotiated channel definition. Wider-channel
CSA wrapper elements will need to be handled when 40/80/160 MHz operation
is enabled. Spectrum Management and Extended Channel Switch action frames
are also not yet handled; this first implementation follows CSA elements
from associated-BSS beacons.

The `MIKE` kernel builds successfully with this implementation. Runtime
validation still requires booting the new kernel and inducing or waiting
for another AP channel change. With DEBUG enabled, the expected messages
are:

```text
mwx0: AP announced switch to channel 104, mode ..., count ...
mwx0: switched to channel 104
```

The interface should remain in RUN and bound gateway traffic should resume
after only the channel-switch interruption, without a firmware reload or
new authentication exchange.
