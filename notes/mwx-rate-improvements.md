# mwx Rate and Throughput Improvements

## Scope

This note investigates low throughput with the OpenBSD `mwx(4)` driver on a
MediaTek MT7922/RZ616:

```text
Upload:    approximately 4.5 Mbit/s
Download:  approximately 13.2 Mbit/s
```

The test interface was associated on 5 GHz channel 100 at approximately
`-50 dBm`:

```text
media: IEEE802.11 autoselect (OFDM6 mode 11a)
ieee80211: nwid EE-C75NQ7 chan 100 bssid b8:6a:f1:bf:de:71 -50dBm
```

The short conclusion is that the driver is limited by more than the absence
of wide channels. It currently negotiates a legacy 802.11a link, leaves the
firmware's AP station record in a preliminary state, has placeholder legacy
rate-control data, does not report the actual firmware-selected TX rate, and
does not enable QoS or frame aggregation.

## Confirmed Findings

### The link is legacy-only

The channel setup in `mwx_preinit()` advertises 2 GHz channels as CCK/OFDM
and 5 GHz channels as 802.11a. The HT, 40 MHz, VHT, and 80 MHz flags are
present only in TODO comments:

```c
/* TODO 11n and 11ac flags:
 * IEEE80211_CHAN_HT | IEEE80211_CHAN_40MHZ |
 * IEEE80211_CHAN_VHT
 * ic_xflags |= IEEE80211_CHANX_80MHZ
 */
```

See `sys/dev/pci/if_mwx.c` around `mwx_preinit()`.

The corresponding device capabilities are also compiled out:

```c
#if NOTYET
	ic->ic_htcaps = ...
#endif

#if NOTYET
	ic->ic_vhtcaps = ...
#endif
```

As a result, net80211 cannot negotiate 802.11n or 802.11ac. The link uses
20 MHz legacy OFDM, whose possible PHY rates are:

```text
6, 9, 12, 18, 24, 36, 48, and 54 Mbit/s
```

This alone limits throughput substantially, but it does not fully explain a
4.5 Mbit/s upload. A healthy legacy 54 Mbit/s link should exceed that even
without 40 or 80 MHz channels.

### The firmware has a separate record for the AP

There are two copies of peer state:

1. Net80211 maintains a software `struct ieee80211_node`. It contains the
   AP's MAC address, association ID, rates, channel, signal strength, QoS
   state, and negotiated PHY capabilities.
2. MediaTek firmware maintains a station and wireless table entry indexed by
   a WCID, or Wireless Client ID.

Although the local machine is operating in station mode, firmware treats the
remote AP as a station-table peer. A normal data TX descriptor identifies a
WCID. Firmware consults the corresponding record to choose rates, retries,
encryption, and aggregation behavior.

`mwx_node_alloc()` assigns a WCID to the net80211 node and initializes its
wireless table entry. The allocation is currently marked as incorrect:

```c
/* XXX this is just wrong */
static uint32_t wcid = 0;
...
mn->wcid = 1 + wcid++ % (MWX_WTBL_STA - 1);
```

This is a separate lifecycle issue that should eventually be corrected, but
the assigned WCID is sufficient for basic traffic in the current test.

### The AP record is created before association

During the transition to `IEEE80211_S_AUTH`, `mwx` creates a firmware station
record for the AP:

```c
case IEEE80211_S_AUTH:
	rv = mt7921_set_channel(sc);
	...
	mt7921_mac_sta_update(sc, sc->sc_ic.ic_bss, 1, 1);
	break;
```

This preliminary record is necessary so the card can transmit authentication
and association management frames.

At this stage, association has not completed. Final information such as the
association ID, negotiated rate set, and negotiated QoS and PHY capabilities
is not yet available. Creating an initial station record in a `NONE` state is
normal; Linux does the same during station allocation.

### The AP record is not updated after association

After the AP accepts association, net80211 parses the association response in
`ieee80211_recv_assoc_resp()`. It records:

- The association ID, or AID.
- The mutually supported legacy rates.
- WMM/QoS information.
- HT, VHT, and HE information when supported by the driver.
- The final operating mode.

Net80211 then asks the driver to enter `IEEE80211_S_RUN`.

The `mwx` RUN transition currently updates the BSS record and creates the
broadcast/group station record:

```c
rv = mt7921_mcu_uni_add_bss(sc, 1);
if (rv)
	break;
rv = mt7921_mac_sta_update(sc, NULL, 1, 1);
```

The `NULL` station update is for the reserved broadcast/group WCID. It is not
an update of the AP's WCID.

Consequently, the AP's firmware station record remains in the preliminary
form created during `AUTH`.

Linux has a distinct association event which updates the AP record:

```c
mt7921_mcu_sta_update(dev, sta, vif, true,
    MT76_STA_INFO_STATE_ASSOC);
```

The relevant firmware state values are:

```text
MT76_STA_INFO_STATE_NONE  = 0
MT76_STA_INFO_STATE_AUTH  = 1
MT76_STA_INFO_STATE_ASSOC = 2
```

OpenBSD currently always sends state zero:

```c
state->state = /* XXX sta_state */ 0;
```

Updating the existing AP WCID with state `ASSOC` and the finalized peer
information is the most important legacy-rate correctness improvement.

### Legacy rate-control data contains placeholders

`mt7921_mcu_add_sta_tlv()` constructs the firmware PHY and rate-adaptation
records. Important fields are currently hard-coded:

```c
phy->basic_rate = htole16(0x0150); /* XXX */
phy->phy_type = mt7921_get_phy_mode_v2(sc, ni);
phy->rcpi = 0xdc; /* XXX STOLEN FROM LINUX DUMP */

supp_rates = RA_LEGACY_OFDM;
ra_info->legacy = htole16(supp_rates);

state->state = /* XXX sta_state */ 0;
```

These fields have different purposes:

- `basic_rate` describes the BSS's mandatory rates. These are commonly used
  for management, broadcast, multicast, and conservative transmissions.
- `legacy` describes the legacy rates supported by the peer and therefore
  available to firmware rate control for unicast traffic.
- `rcpi` gives firmware a signal-strength estimate for the peer.
- `state` tells firmware how far the peer has progressed through the
  connection lifecycle.

The current legacy mask enables every OFDM rate rather than translating the
negotiated `ni->ni_rates` set. On a typical 5 GHz AP this may happen to be
mostly correct, but it is not guaranteed to match the association.

The basic-rate bitmap should be derived from the AP's negotiated basic rates
using the firmware's band-specific rate-table indexing. The value `0x0150`
should not be retained without confirming its exact meaning for MT7921/MT7922.

RCPI should be calculated from a measured signal value rather than copied
from a firmware dump. The current interface reports roughly `-50 dBm`, but
that measurement is not passed to the firmware station update.

### HT/VHT peer rate information is absent

The firmware station record has fields for HT and VHT capabilities, MCS
masks, bandwidth, NSS, and A-MPDU parameters. Their population is currently
under `NOTYET`:

```c
#ifdef NOTYET
	/* sta rec ht */
	...
	/* sta rec vht */
	...
	memcpy(ra_info->rx_mcs_bitmask, ...);
	state->vht_opmode = ...;
#endif
```

Therefore, enabling only the HT/VHT channel flags would be unsafe. Net80211
could negotiate a mode that is not represented correctly in the firmware
peer record.

### QoS and aggregation are disabled

The device capability flags for QoS, TX A-MPDU, and firmware ADDBA handling
are under `NOTYET`:

```c
#if NOTYET
	IEEE80211_C_QOS |
	IEEE80211_C_TX_AMPDU |
	IEEE80211_C_ADDBA_OFFLOAD |
#endif
```

`netstat -W mwx0` reported zero input and output block-ack agreements. This is
consistent with the capability flags.

Without A-MPDU, each data frame has its own contention, inter-frame spacing,
PHY preamble, acknowledgement, and encryption overhead. This is a major
throughput limitation even at high PHY rates.

Hardware A-MSDU is also disabled in the TX descriptor:

```c
txp->txwi[7] =
    /* XXX wcid->amsdu ? htole32(MT_TXD7_HW_AMSDU) : */ 0;
```

### Driver TX-rate reporting is unfinished

The interface reports:

```text
OFDM6 mode 11a
```

This is not conclusive proof that firmware sends every frame at 6 Mbit/s.
Net80211 initializes the node's legacy TX-rate index to zero, which is OFDM6
on 5 GHz. The driver's association callback does not update it:

```c
/* XXX TODO rate handling here */
```

Firmware TX-status packets are also discarded rather than translated into
net80211 rate-control or reporting updates. As a result, `ifconfig` can show
OFDM6 even if firmware has internally selected another rate.

Nevertheless, approximately 4.5 Mbit/s of TCP payload is strongly consistent
with a 6 Mbit/s PHY rate after 802.11 and TCP/IP overhead. The actual
over-the-air rate should be verified with an external monitor or AP-side
per-client statistics.

## Why Upload Is Worse Than Download

On upload, the MediaTek device chooses a transmit rate for the AP's WCID.
That decision depends on the incomplete firmware station and rate-adaptation
record described above.

On download, the AP chooses its own transmit rate. The local device reads the
rate and modulation from each frame's PHY preamble and decodes it. The AP has
its own mature rate-control implementation and can choose a faster legacy
rate than the local firmware may be using for upload.

The AP must still respect the capabilities advertised by `mwx`, so download
also remains limited to the legacy link. This explains why download is
better than upload but still only about 13 Mbit/s.

## Recommended Implementation Order

### Stage 1: Make the legacy association record correct

Keep the link in legacy 20 MHz mode initially. This isolates station-record
and rate-control changes from HT/VHT and aggregation work.

The firmware station-update API should accept an explicit station state.
The expected lifecycle is approximately:

```text
AUTH:
    create AP WCID with state NONE and newly=true

RUN:
    update BSS information
    update the same AP WCID with state ASSOC and newly=false
    initialize or update the reserved broadcast/group WCID
```

The exact ordering and `newly` semantics should follow the Linux MT7921
implementation and be checked against key installation and reassociation.
The association update must not accidentally allocate a different WCID.

The final AP update should include:

- The AID from `ni->ni_associd`.
- The AP MAC address.
- Final QoS state.
- The negotiated supported legacy rate mask.
- The negotiated basic-rate mask.
- PHY type for the selected band.
- RCPI derived from a real RSSI measurement.
- Station state `MT76_STA_INFO_STATE_ASSOC`.

This stage may improve upload without advertising any new 802.11 mode.

### Stage 2: Make TX-rate behavior observable

Implement enough TX-status processing to determine:

- The initial and final rate used for each transmission.
- Retry count.
- Success or failure.
- The WCID associated with the result.

Update net80211's displayed node rate where appropriate. At minimum, add
debug counters or rate histograms so firmware rate behavior can be verified
without relying on the default `ni_txrate`.

This work is important for diagnosis, although firmware-offloaded rate
control may not require host feedback to operate.

### Stage 2a: Fix TX backpressure and ownership

TX resource handling should be corrected before enabling HT or aggregation.
Higher PHY rates and A-MPDU will increase the rate at which the host submits
packets and make the existing failure paths easier to trigger.

There are two finite resources on the normal data path:

- The pool contains `MWX_TXWI_MAX`, currently 512, TXWI entries.
- The data DMA ring contains 256 descriptors, with one descriptor used for
  each submitted TXWI.

`mwx_start()` does not check either resource before removing a packet from
`if_snd` or the management queue. It has a placeholder for setting
`if_snd` active, but never calls `ifq_set_oactive()`. It therefore continues
dequeueing after the ring or TXWI pool has become full.

The resulting error handling does not have a consistent packet-ownership
contract:

1. `mwx_tx()` removes a TXWI from `mt_freelist` and marks it busy.
2. If `mwx_txwi_enqueue()` cannot load the packet DMA map, `mwx_tx()` returns
   without calling `mwx_txwi_put()`. The TXWI remains busy and is permanently
   removed from the free list.
3. If the packet map succeeds but `mwx_dma_txwi_enqueue()` finds the data
   ring full, the TXWI already owns the mbuf and a loaded DMA map. The TXWI
   was never submitted to hardware, so no TX-free notification will arrive
   to release it.
4. The ring-full branch calls `bus_dmamap_unload()` on the DMA map belonging
   to the current queue slot even though that map was not loaded for this
   TXWI submission. The loaded map which actually needs unwinding belongs to
   the TXWI.
5. `mwx_start()` releases the node reference and increments `if_oerrors` when
   `mwx_tx()` fails, but it neither frees nor requeues the mbuf. Repeated
   failures can therefore leak both packets and TXWI entries while the start
   loop continues to dequeue traffic.

There is also an ownership problem on the successful path. Net80211 returns
a referenced `ieee80211_node` with an encapsulated data frame, and management
frames carry their referenced node in `m_pkthdr.ph_cookie`. Unlike drivers
such as `iwx`, `mwx` does not retain that pointer in its per-packet TX state
and does not release it when `mwx_mac_tx_free()` completes the packet.

Resource exhaustion should be handled before dequeueing a packet. A suitable
design is:

- Track the number of available TXWI entries and data-ring descriptors.
- Stop dequeueing and call `ifq_set_oactive()` when either resource reaches a
  high-water threshold.
- Reserve both resources before committing packet ownership to the TX path.
- Once `mwx_tx()` accepts a packet, make the TXWI the unambiguous owner of the
  mbuf, DMA map, and node reference until completion or reset.
- Unwind partial setup in reverse order on every error: unload the packet map
  if loaded, detach or free the mbuf according to the caller contract,
  release the node reference, clear the descriptor, and return the TXWI to
  the free list.
- When TX completion returns enough resources, clear `oactive` and invoke
  `if_start` to resume both management and data traffic. A low-water
  threshold should be used to avoid repeatedly stopping and restarting for
  one descriptor.
- During stop or reset, drain every busy TXWI and release the same resources
  as the normal completion path.

An ordinary full-ring condition should not be treated as an output error.
It is expected flow control. Since a data packet has already been transformed
from Ethernet to 802.11 after `ieee80211_encap()`, avoiding dequeue until
resources are available is simpler and safer than attempting to put an
encapsulated packet back on `if_snd`.

The initial upload test also showed why queue behavior needs to be measured
separately from PHY rate. `iperf3` queued 27.1 MBytes in approximately 10
seconds at a reported sender rate of 22.7 Mbit/s, while the receiver took
20.23 seconds and reported 11.3 Mbit/s. This does not prove that the driver
queue caused the entire delay, since TCP socket buffers can also absorb a
short test, but it demonstrates that sender throughput cannot be used as the
delivered throughput while a large queue is still draining.

### Stage 3: Enable HT and VHT coherently

Only after the legacy station update works reliably:

- Advertise HT capability on supported 2 GHz and 5 GHz channels.
- Advertise VHT capability on supported 5 GHz channels.
- Enable 40 and 80 MHz flags according to hardware, regulatory, and channel
  constraints.
- Populate `STA_REC_HT` and `STA_REC_VHT`.
- Populate the negotiated HT MCS mask.
- Populate VHT RX and TX MCS maps.
- Populate bandwidth and NSS in `vht_opmode`.
- Populate A-MPDU factor and density.
- Respect user configuration such as disabling MIMO or fixing a rate.

The MT7922 hardware supports more than the initial implementation needs.
Starting with HT20, then HT40, then VHT80 provides smaller testable steps.

### Stage 4: Enable QoS and A-MPDU

QoS is required before normal data A-MPDU operation. Implement and test:

- WMM access-category mapping rather than the current hard-coded queue.
- Per-TID TX handling.
- RX and TX block-ack setup and teardown.
- Firmware BA commands equivalent to Linux's `mt7921_mcu_uni_rx_ba()` and
  `mt7921_mcu_uni_tx_ba()`.
- Net80211 A-MPDU callbacks or the correct firmware-offload integration.
- BAR handling, sequence numbers, and teardown on reassociation.

Aggregation is likely to provide a larger throughput improvement than moving
from 20 to 40 MHz by itself because it removes repeated per-frame overhead.

### Stage 5: Enable A-MSDU and improve the TX queue

Once A-MPDU is stable, add A-MSDU station capability and enable
`MT_TXD7_HW_AMSDU` only for eligible traffic.

TX backpressure and ownership are described in Stage 2a and should already
be correct before reaching this stage. A-MSDU adds another ownership layer
because one hardware submission may represent multiple packets, so it should
reuse the same completion and reset invariants rather than add a separate
cleanup path.

## Verification Plan

Each stage should be tested independently on a fixed AP, band, channel, and
signal level.

For the legacy station-record stage:

- Confirm association, WPA2 key installation, IPv4, IPv6, and multicast.
- Run upload, reverse, bidirectional, TCP, and UDP `iperf3` tests.
- Record driver TX success, retry, and selected-rate information.
- Compare AP-side client TX and RX rates.
- Use an external monitor capture to verify the actual OpenBSD TX PHY rate.

For HT/VHT:

- Confirm `ifconfig` reports 11n or 11ac only after firmware setup succeeds.
- Confirm negotiated channel width and NSS.
- Test HT20 before HT40 and VHT80.
- Confirm legacy APs and 2 GHz associations continue to work.

For aggregation:

- Confirm `netstat -W mwx0` shows block-ack agreements.
- Verify QoS data uses the expected TID and access category.
- Check for duplicate frames, BA-window gaps, retries, and BAR storms.
- Test with WPA2 because OpenBSD intentionally avoids TX A-MPDU on
  unencrypted networks.

For TX backpressure and ownership:

- Run long upload, reverse, and bidirectional tests rather than relying on a
  short test which can finish while TCP and driver queues are still draining.
- Compare sender and receiver byte counts, throughput, and duration after all
  queued traffic has drained.
- Record TXWI free, busy, submitted, and completed counts and verify that
  `free + busy` remains equal to the configured pool size.
- Record ring-full, TXWI-empty, queue-stop, and queue-restart events. Ring
  saturation should stop dequeueing without increasing `if_oerrors`.
- Verify that every accepted packet produces exactly one completion or is
  reclaimed by stop/reset, and that every node reference is released.
- Exercise resource exhaustion by temporarily reducing the ring or TXWI pool
  size, and exercise DMA-map failures with fault injection if available.
- Run ping concurrently with a saturated upload to measure latency and check
  that high/low watermarks prevent an excessive queue-drain tail.
- Repeat interface down/up, reassociation, suspend/resume, and reset while
  traffic is queued, checking for leaked TXWI entries, DMA maps, mbufs, and
  node references.

Regression testing should include suspend/resume, background scanning,
reassociation, roaming between BSSIDs, key replacement, and repeated
interface down/up cycles.

## Expected Impact

The first goal is not immediately reaching the hardware's advertised Wi-Fi 6
rate. It is making legacy 802.11a transmission correct and measurable.

If the current upload is actually using OFDM6, a valid associated station
record should allow firmware to select higher legacy rates under the observed
`-50 dBm` signal. That could improve upload substantially while remaining on
a 20 MHz legacy link.

HT/VHT and wider channels raise the available PHY rate, but QoS and A-MPDU
are essential for converting that PHY rate into useful TCP throughput.
Accordingly, channel width should not be treated as the only or first
throughput improvement.
