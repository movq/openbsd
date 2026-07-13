# mwx IPv6 and Multicast Debugging

## Summary

The experimental OpenBSD `mwx(4)` driver works for IPv4 and unicast Wi-Fi
traffic on this machine, but it does not receive inbound Wi-Fi
group-addressed traffic. IPv6 fails as a consequence because return-path
Neighbor Discovery uses solicited-node multicast.

The initial symptom looked like an IPv6 routing problem:

- SLAAC configured global IPv6 addresses.
- A default IPv6 route was installed.
- The router's link-local address was reachable.
- External IPv6 destinations were not reachable.

Packet captures and tests from another WLAN station showed that the more
general problem is missing inbound multicast. Three kernel changes have been
tested without changing the behavior:

1. Enabling the MT7921/MT7922 firmware RX filter like Linux.
2. Falling back to net80211 software crypto for the GTK.
3. Disabling hardware RX header translation.

The next useful step is to instrument the earliest RX path and inspect the
RX descriptor and RFCR state for group frames, rather than trying further
unverified fixes.

## Test Environment

- Machine: Framework laptop
- OS: OpenBSD 7.9-current
- Kernel configuration: `MIKE`
- Current test kernel:

```text
OpenBSD mike-framework-obsd.local 7.9 MIKE#2 amd64
OpenBSD 7.9-current (MIKE) #2: Mon Jul 13 13:51:22 BST 2026
```

- Wi-Fi device:

```text
mwx0 at pci1 dev 0 function 0 "MediaTek RZ616" rev 0x00: msi,
    rev: MT7922.10
PCI vendor 14c3, product 0616
```

- Interface MAC: `14:ac:60:46:7b:bd`
- AP BSSID: `b8:6a:f1:bf:de:71`
- Router/AP Ethernet MAC used for data: `b8:6a:f1:bf:de:70`
- Network: `EE-C75NQ7`
- Security: WPA2-PSK with CCMP pairwise and group ciphers
- Interface MTU: 1492
- Linux mt76 reference:
  `/home/mike/src/linux/drivers/net/wireless/mediatek/mt76`
- OpenBSD working tree:
  `/home/mike/src/openbsd-src`
- Kernel build tree:
  `/usr/src/sys/arch/amd64/compile/MIKE`

The other test machine is a Linux WLAN station:

```text
hostname: DE-C-001VK
interface: wlp0s20f3
MAC: c8:5e:a9:0e:05:fb
IPv4: 192.168.1.101
```

It is associated with the same BSSID and is reachable from OpenBSD with
unicast IPv4.

## Working Configuration

`mwx0` successfully associates and passes unicast traffic. A representative
configuration is:

```text
mwx0: flags=<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST,AUTOCONF6TEMP,
    AUTOCONF6,AUTOCONF4> mtu 1492
lladdr 14:ac:60:46:7b:bd
inet 192.168.1.68 netmask 0xffffff00
inet6 fe80::16ac:60ff:fe46:7bbd%mwx0 prefixlen 64
inet6 2a00:23c7:76ce:1501:edde:10a5:15e6:2f44 prefixlen 64 autoconf
```

The router advertisement installs a valid default route:

```text
default fe80::ba6a:f1ff:febf:de70%mwx0 UGS ... mwx0
```

The router's neighbor entry resolves to:

```text
fe80::ba6a:f1ff:febf:de70%mwx0 b8:6a:f1:bf:de:70
```

Working tests:

```sh
ping 192.168.1.101
ping6 fe80::ba6a:f1ff:febf:de70%mwx0
```

Failing tests:

```sh
ping6 2606:4700:4700::1111
ping6 google.com
```

## Packet-Capture Evidence

An external ping with a fixed stable source address was captured using:

```sh
doas tcpdump -n -e -vvv -i mwx0 'icmp6'
ping6 -c 3 -I 2a00:23c7:76ce:1501:edde:10a5:15e6:2f44 \
    2606:4700:4700::1111
```

The outgoing echo requests appeared:

```text
14:ac:60:46:7b:bd > b8:6a:f1:bf:de:70, ethertype IPv6:
2a00:23c7:76ce:1501:edde:10a5:15e6:2f44 >
2606:4700:4700::1111: ICMP6 echo request
```

No echo replies appeared.

Seeing an outgoing packet in this capture does not prove that the hardware
transmitted it. `mwx_start()` taps outgoing Ethernet packets before
`ieee80211_encap()` and before DMA submission. In this case, however, working
unicast IPv4 and router pings show that the general unicast TX path works.

The same capture showed successful unicast Neighbor Discovery with the
router:

```text
b8:6a:f1:bf:de:70 > 14:ac:60:46:7b:bd:
fe80::ba6a:f1ff:febf:de70 >
fe80::16ac:60ff:fe46:7bbd: neighbor solicitation
```

Although this is an ICMPv6 Neighbor Solicitation, its Ethernet destination
is unicast. It does not test multicast reception.

## Return-Path Neighbor Discovery

From the Linux WLAN station:

```sh
ping6 2a00:23c7:76ce:1501:edde:10a5:15e6:2f44
```

Linux reported:

```text
Destination unreachable: Address unreachable
```

The corresponding solicited-node Neighbor Solicitation did not appear in
the OpenBSD capture. The Linux neighbor entry for the OpenBSD global address
eventually entered `FAILED`.

This explains the original external IPv6 symptom. The router cannot deliver
external replies until it resolves the OpenBSD global address. Its
solicited-node multicast NS is not received, so it never learns the mapping
between the global IPv6 address and `14:ac:60:46:7b:bd`.

## All-Nodes Multicast Test

The Linux station sent:

```sh
ping -6 -c 4 -I wlp0s20f3 ff02::1
```

Linux received replies from the router and several other LAN devices. It did
not receive a reply from OpenBSD. The simultaneous OpenBSD ICMPv6 capture
printed no packets from this test.

This is important because it rules out a problem limited to one IPv6
solicited-node membership. The OpenBSD station does not receive ordinary
all-nodes multicast either.

The established failure is therefore:

```text
Inbound Wi-Fi group-addressed traffic does not reach the OpenBSD network
stack, while unicast traffic from the same WLAN works.
```

## Other Checks

### Routing and Address Selection

The IPv6 address, connected prefix route, default route, and router neighbor
entry are present. Tests forced the stable source address, so temporary
address selection is not the cause.

### PF

The active PF rules included:

```text
block return all
pass all flags S/SA
```

There was no relevant PF block. More importantly, missing frames did not
appear in `tcpdump`, placing the failure before PF.

### MTU

The failing echo requests were much smaller than the 1492-byte interface
MTU. Path MTU discovery is not needed for these packets.

### Checksum Offload

The `mwx` RX checksum-offload code is under `NOTYET` in
`mt7921_mac_fill_rx()`. ICMPv6 checksum errors were not observed. This is not
a likely checksum-offload failure.

### AP Isolation

The Linux WLAN station and OpenBSD can exchange unicast IPv4, including SSH
and ICMP. The AP also forwards the Linux all-nodes ping to multiple other
devices. General wireless client isolation does not explain the result.

## Linux RX-Filter Comparison

Linux always enables the MT7921 receive filter from
`mt7921_configure_filter()`:

```c
u32 flags = MT7921_FILTER_ENABLE; /* BIT(31) */
mt7921_mcu_set_rxfilter(dev, flags, 0, 0);
```

It sends CE command `SET_RX_FILTER`, command ID `0x0a`, with a packed
64-byte request. OpenBSD had no equivalent command and contains this TODO:

```c
/*
 * mcu_uni_add_dev, mwx_mac_wtbl_update,
 * mcu_set_rxfilter_enable, mcu_radio_on_off_ctrl
 */
```

Relevant OpenBSD definitions include:

```c
MT_WF_RFCR_DROP_MCAST
MT_WF_RFCR_DROP_BCAST
MT_WF_RFCR_DROP_MCAST_FILTERED
```

OpenBSD records multicast memberships through `ieee80211_ioctl()` and
`ether_addmulti()`, but `mwx_ioctl()` does not program a multicast-address
filter.

## Tested Kernel Changes

### 1. Firmware RX-Filter Enable

The Linux `mt7921_mcu_set_rxfilter()` request and
`MCU_CE_CMD_SET_RX_FILTER` (`0x0004000a`) were ported to OpenBSD. The command
was sent during normal `mwx_init()` with `BIT(31)`.

The running kernel was verified to contain the new function, and `dmesg`
showed:

```text
mwx0: mwx_mcu_send_mbuf: cmd 0004000a
```

Result: no change. Solicited-node and all-nodes multicast remained absent.

This code was removed before the next isolated test.

### 2. Software GTK Decryption

The group-key path was changed to use `ieee80211_set_key()` and
`IEEE80211_KEY_SWCRYPTO`, while retaining hardware PTK offload for unicast.
The intent was to determine whether group-key installation on WCID 19 was
incorrect.

Result: no change. No inbound group frames reached tcpdump or net80211.

This does not completely exonerate GTK handling. With no hardware GTK, the
firmware may discard encrypted group frames before host software can decrypt
them. The experiment only showed that the current hardware/firmware path
does not transparently pass undecrypted group frames to net80211.

This code was removed before the next isolated test.

### 3. Disable RX Header Translation

Commit `9b063aa2ab0` changed shared MAC initialization to enable:

```c
MT_MDP_DCR0_RX_HDR_TRANS_EN
```

At the same time, OpenBSD's `mt7921_mac_fill_rx()` rejects any packet whose
RX descriptor has `MT_RXD2_NORMAL_HDR_TRANS`.

The AP station WCID is configured with `no_rx_trans = 1`, which could explain
why unicast survives while the group WCID behaves differently. The test
kernel explicitly cleared global header translation:

```c
/* net80211 expects 802.11 frames; translated frames are rejected. */
if (sc->sc_hwtype != MWX_HW_MT7925)
	mwx_clear(sc, dcr0, MT_MDP_DCR0_RX_HDR_TRANS_EN);
```

The running `MIKE#2` kernel was verified to contain this change and not the
two earlier experiments.

Result: no change. Solicited-node and all-nodes multicast remained absent.

This is the only current workspace modification:

```text
M sys/dev/pci/if_mwx.c
```

## Group-Key and WCID Notes

The OpenBSD group key is installed on the per-interface reserved WCID:

```c
sc->sc_vif.vif_mn.wcid = MWX_WTBL_RESERVED; /* 19 */
```

For a group key, `mt7921_mcu_sta_key_update()` uses:

```c
wcid = mvif->vif_mn.wcid;
muar_idx = 0x0e;
```

The pairwise key uses the AP peer node's WCID.

This broadly matches Linux. For a normal managed-station GTK, mac80211 links
the key without a station, and `mt7921_set_key()` selects
`mvif->sta.deflink.wcid`, the reserved interface WCID.

Potential differences still worth checking:

- Whether OpenBSD creates all required station-record and WTBL TLVs for the
  reserved group WCID before installing the GTK.
- Whether the RX descriptor reports WCID 19 for multicast frames.
- Whether the firmware accepts the GTK update but associates it with the
  wrong BSS or MUAR index.
- Whether the current `STA_REC_KEY` request is sufficient for MT7922.10 and
  this firmware revision.

## Current Source and Build State

The source trees are distinct:

```text
/home/mike/src/openbsd-src
/usr/src
```

For `MIKE#2`, `/usr/src/sys/dev/pci/if_mwx.c` was synchronized with the
header-translation change from the workspace. The running kernel has no
`mt7921_mcu_set_rxfilter` symbol, confirming that the earlier experiment is
not present.

The current workspace diff passes:

```sh
git diff --check -- sys/dev/pci/if_mwx.c sys/dev/pci/if_mwxreg.h
```

## Recommended Next Steps

### Instrument RX Drop Reasons

Add counters or rate-limited diagnostics at the earliest point where normal
RX descriptors are dequeued. At minimum, count and print:

- RX packet type
- RXD0 through RXD4
- WLAN index
- unicast versus multicast address type
- security mode and key ID
- `ICV_ERR`
- `FCS_ERR`
- `AMSDU_ERR`
- `HDR_TRANS`
- maximum-length and header-translation errors

Diagnostics must happen before `mt7921_mac_fill_rx()` returns an error.
Current Ethernet and radiotap BPF taps occur too late to show frames rejected
there.

Run the instrumentation while the Linux station sends `ff02::1`. This will
show whether group frames enter host DMA and exactly why they are rejected.
If no RX descriptor appears, the drop occurs in firmware, hardware filtering,
or GTK processing before host DMA.

### Read and Test RFCR Directly

Log `MT_WF_RFCR(0)`:

- After firmware startup
- After `MCU_CE_CMD_SET_RX_FILTER`
- After association and key installation

Check whether any of these are set:

```c
MT_WF_RFCR_DROP_MCAST
MT_WF_RFCR_DROP_BCAST
MT_WF_RFCR_DROP_MCAST_FILTERED
```

As a controlled diagnostic, directly clear only those bits after association
and log the value read back. The earlier MCU-command experiment did not
prove that the firmware command changed RFCR.

### Capture Over the Air

Use a second adapter in monitor mode on the AP channel to verify:

- The AP transmits the Linux station's `ff02::1` packet over the air.
- Whether it transmits group traffic as native multicast or
  multicast-to-unicast.
- The 802.11 receiver address, protected bit, CCMP key ID, and BSSID.

This separates an AP forwarding decision from a receive-side driver drop.

### Test an Open Network

Testing `mwx` on an open AP would cleanly separate GTK/decryption from generic
multicast RX. Nearby scans showed an open `EE WiFi` SSID, although it may use
a captive portal and may not bridge clients. A controlled Linux hotspot or
test AP without WPA would be preferable.

### Validate the Reserved Group WCID

Compare the complete Linux and OpenBSD BSS/group-WCID setup, not only the key
TLV. Useful diagnostics include:

- Initializing or dumping WCID 19 before and after BSS setup.
- Dumping the key-related station record after GTK installation.
- Temporarily installing the GTK on both WCID 19 and the AP peer WCID to test
  WCID selection.
- Verifying `bmc_tx_wlan_idx`, `sta_idx`, `bss_idx`, and `muar_idx`.
