# Personal Hotspot on the S20 — status and handoff (2026-11-19)

**WPA2 works (17:02):** iPhone clients complete our four-way handshake (`CLIENT SECURED (WPA2)`) and browse.

Branch `feat/s20-hotspot` (worktree `work/hackios-hotspot`), from `x1slte`. Open network works end to end:
a Mac, an iPhone and the S20 itself browsed over Jio LTE through the phone (2026-11-19 14:35).

## How it works

| Piece | Where | What |
|---|---|---|
| Access point | `xnu/files/bsd/dev/redwood_bcm4375.c` (`ap_start`, `ap_stop`) | The BCM4375 as an AP (brcmfmac's start_ap sequence). Confirmed by E_LINK (firmware 18.41.117 sends no E_SET_SSID). A flow ring towards broadcast, en0's link up. Client mode (wifid) refused with EBUSY while it runs. Switch: `sysctl -w kern.redwood.wifi_ap="start CH NAME [PASS]"` / `=stop` (tabs instead of spaces for names with spaces). |
| IPv4 over IPv6-only Jio | `xnu/redwood.patch` (bsd/net/dlil.c) | iOS's own CLAT46 on the cellular utun. Fix: a utun keeps its 4-byte AF header until utun_proto_input, so CLAT46 read the IPv6 header 4 bytes early and dropped every packet ("too short"). |
| Autonomous prefix | `tools/cellular/live-modem.c` (`tun_open`) | The data address is added with IN6_IFF_AUTOCONF: CLAT46 needs an autonomous /64. |
| CLAT46 switch-on | `tools/hotspot/clat-test.c` | `clat-test start utunN 64:ff9b::/96` (Jio's NAT64 prefix, found with `clat-test nat64 2405:200:800::1`). Then 192.0.0.2 on the utun and an IPv4 default route. |
| DHCP | `tools/hotspot/rwdhcpd.c` | iOS's bootpd exits outside Personal Hotspot. Ours: 172.20.10.2–14, replies as raw frames via BPF, only on en0, leases in /var/db/rwdhcpd.leases. |
| NAT | `tools/hotspot/rwpf.c` | iOS ships no pfctl. A scrub rule (max-mss 1400: CLAT46 adds 20 bytes, full-size packets stalled a client's first pages) and one NAT rule through /dev/pf + IPv4 forwarding. Build with `-I` a folder holding xnu-src's net/pfvar.h, net/radix.h, libkern/tree.h. |
| All of it | `tools/hotspot/hotspot-up.sh start [NAME [CH]] | stop | status` | Bring-up by hand (binaries in /var/root, network-cmds from Procursus in /var/jb). |

## Lessons

- `networksetup -setairportnetwork` on macOS 15 cannot see network names over SSH (no location permission): switch
  the Hackintosh by hand when testing it as a client. Its only network is Wi-Fi, so joining the hotspot cuts SSH.
- Fixed (cb0719b9): restarting the cellular service left its old utun alive with the default route (the IMS client,
  started with posix_spawn, inherited the tunnel). The tunnels are close-on-exec now; verified: `echo restart >
  /var/tmp/redwood-cell/cmd` keeps the same interfaces and data comes back by itself. After it, run hotspot-up.sh start.
- Many back-to-back reboots overheat the phone (PMIC reset, black screen): use the cooler.

## Next

1. ~~WPA2~~ done: lx_wpa_ap_* (host-tested: tools/wpa-test, 489 checks) + ap_thread/ap_eapol in the driver; RSN
   capabilities from iovar wpa_cap so message 3 matches the beacon. `hotspot-up.sh start NAME CH PASSWORD`.
2. **Automatic**: the cellular service switches CLAT46 on by itself; a launchd job runs hotspot-up.sh; the phone's
   own DHCP client on en0 stopped while the AP runs (it asks for an address on its own AP: harmless, 169.254 noise).
3. **Settings → Personal Hotspot**: iOS's switch, name and password (misd/CommCenter answers).
