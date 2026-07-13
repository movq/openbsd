# mwx IPv6 and Multicast Debugging

## Summary

The inbound multicast failure has been diagnosed and fixed. Two firmware
records were missing from the OpenBSD association sequence:

1. The associated BSS was not updated with its BSSID, beacon/DTIM values,
   PHY mode, QoS state, and reserved BMC WCID.
2. Reserved group WCID 19 received the GTK but had no station-record or WTBL
   GENERIC/RX/HDR_TRANS setup.

Adding only the Linux-style association-time `BSS_INFO_UPDATE` made group
frames reach host DMA, but they arrived with invalid WCID 1023, security mode
0, and `HDR_TRANS_ERROR`. Initializing WCID 19 before GTK installation made
the same frames report WCID 19, CCMP security mode 4, GTK key ID 1, and no RX
descriptor errors.

The final test kernel receives the Linux station's `ff02::1` echo requests,
answers solicited-node Neighbor Discovery, and passes external IPv6 traffic.
The earlier symptom looked like an IPv6 routing problem even though SLAAC,
the default route, and unicast traffic were all working.

Three earlier experiments did not change the behavior:

1. Enabling the MT7921/MT7922 firmware RX filter like Linux.
2. Falling back to net80211 software crypto for the GTK.
3. Disabling hardware RX header translation.

## Test Environment

- Machine: Framework laptop
- OS: OpenBSD 7.9-current
- Kernel configuration: `MIKE`
- Fix-validation kernel:

```text
OpenBSD mike-framework-obsd.local 7.9 MIKE#2 amd64
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
  `/home/mike/src/openbsd-src/sys/arch/amd64/compile/MIKE`

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

That test kernel was verified to contain this change and not the two earlier
experiments.

Result: no change. Solicited-node and all-nodes multicast remained absent.

The header-translation change remains in the final source because net80211
expects 802.11 frames. It became effective for group traffic once WCID 19 was
configured with `no_rx_trans = 1`.

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

This key selection matches Linux. The missing piece was initialization of the
reserved WCID before the key update. OpenBSD now creates its broadcast
station record and WTBL GENERIC/RX/HDR_TRANS TLVs while entering RUN. In
station mode the group WTBL entry uses the AP BSSID, MUAR index `0x0e`, and
`no_rx_trans = 1`.

## Current Source and Build State

The kernel is now built directly from the workspace:

```text
/home/mike/src/openbsd-src
/home/mike/src/openbsd-src/sys/arch/amd64/compile/MIKE
```

`MIKE#2` contains the working BSS and WCID changes plus temporary RX/RFCR
diagnostics. The workspace has since removed those diagnostics while
retaining the functional changes and the correction that treats every
nonzero `mt7921_mac_fill_rx()` result as an RX error.

The cleaned source builds successfully as `MIKE#3` and is installed as
`/bsd`; the validated diagnostic `MIKE#2` is preserved as `/obsd`.

The current workspace diff passes:

```sh
git diff --check -- sys/dev/pci/if_mwx.c sys/dev/pci/if_mwxreg.h
```

## Resolution Evidence

### Early RX and RFCR Instrumentation

The first instrumented kernel counted normal RX descriptors before
`mt7921_mac_fill_rx()`. While the Linux station continuously sent `ff02::1`,
all descriptors were address type 1 (U2M). There were no parser errors or
rejections.

RFCR was `0x00000000` at RUN, pairwise-key installation, and group-key
installation. In particular, none of these were set:

```c
MT_WF_RFCR_DROP_MCAST
MT_WF_RFCR_DROP_BCAST
MT_WF_RFCR_DROP_MCAST_FILTERED
```

This proved that the original drop happened before host DMA and was not
caused by those RFCR bits.

### Association-Time BSS Update

Linux calls `mt76_connac_mcu_uni_add_bss()` when a station associates.
OpenBSD did not have the equivalent call. A port of its BASIC and QBSS update
was added while entering RUN. It supplies:

- The associated AP BSSID.
- Beacon interval and DTIM period.
- PHY mode and non-HT basic PHY capabilities.
- QoS and infrastructure-station connection state.
- Reserved WCID 19 as `bmc_tx_wlan_idx` and `sta_idx`.

After this change, group descriptors immediately reached DMA, but they had:

```text
addr 2/3, wcid 1023, sec 0, HDR_TRANS_ERROR
```

No incoming multicast reached the Ethernet BPF tap, and external IPv6 still
failed.

### Reserved WCID Initialization

OpenBSD previously installed the GTK on WCID 19 without first creating the
group station record and WTBL entries. The RUN transition now calls
`mt7921_mac_sta_update(sc, NULL, 1, 1)` after the BSS update and before key
installation.

With the group WCID initialized, descriptors changed to:

```text
addr 2/3, wcid 19, sec 4, key 1, no RX error
```

This identifies CCMP hardware decryption with the GTK and confirms that the
reserved group receive path is correctly selected.

### Final Network Tests

The Linux station's background all-nodes multicast requests appeared in
OpenBSD tcpdump:

```text
c8:5e:a9:0e:05:fb > 33:33:00:00:00:01:
fe80::5006:60da:ef3f:2af4 > ff02::1: ICMP6 echo request
```

OpenBSD sent unicast echo replies. Solicited-node Neighbor Solicitations for
the stable global address also appeared, followed by OpenBSD Neighbor
Advertisements.

Final tests passed:

```text
Cloudflare IPv6: 5 transmitted, 5 received
Google IPv6:     3 transmitted, 3 received
Linux IPv6:      5 transmitted, 5 received
Linux IPv4:      5 transmitted, 5 received
```
