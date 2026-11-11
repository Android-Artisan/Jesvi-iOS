# Jesvi IOS device identity and development defaults

Fresh builds seed the computer/display name **Jesvi IOS** and the DNS-safe
local hostname **Jesvi-IOS** in the normal SystemConfiguration preferences.
The existing init skeleton copies this seed only when the preferences file
does not exist, preserving later user names and network configuration.
The USB product string and diagnostic Bonjour service name use Jesvi IOS.

For an existing data volume, run `/redwood/bin/device_name --set-default` as
root. This calls configd's name APIs, commits and applies the change while
preserving other network settings. Running without arguments only inspects.
An entitled MobileGestalt read on the live phone confirmed UserAssignedDeviceName
as Jesvi IOS. Unentitled apps may still receive “iPhone” under iOS name privacy;
DeviceName is the model description and is not rewritten.

The AMFI stand-in exposes read-only `security.mac.amfi.developer_mode_status`
and `security.mac.amfi.developer_mode_resolved` as 1 from boot, reflecting the
research kernel's existing development-code policy. This avoids asking native
amfid to arm a hardware-backed mode that this port does not implement.
A temporary amfid-only test first verified the cause. The permanent kernel
then booted with both OIDs equal to 1, no temporary amfid injection, and the
native TrollStore check returning 0. A debugging-entitled test IPA installed
with exit 0 after that reboot and was removed. Developer Mode therefore needs
no per-boot toggle in this port.

The Finder iPhone-style trust/sidebar/eject feature remains separate work.
USB presently supplies CDC serial and Ethernet; the stock lockdownd and AFC
executables are available, but the job recipe excludes lockdown/USB services
and the kernel has no native device-side USB multiplexing interface. The
lockdown launch recipe exposes TCP port 62078, which can be tested over the
existing USB Ethernet link before changing USB descriptors or transport.
No native Finder support is claimed by these name/development changes.

The intermittent cold Safari error is deferred at the user's request. Normal
DNS and HTTPS probes succeeded (DNS ~0.03 s; HTTPS ~0.06–0.09 s), which does
not establish what fails in a cold WebKit process. No speculative Safari or
network workaround has been applied.

## Native Finder feasibility probe

The stock lockdownd was temporarily loaded with a USB-address-only listener,
no keepalive/panic-on-crash policy, and logging. QueryType over TCP 62078 was
reset. Its own logs identify the missing `IOUSBDeviceController`, and explicitly
reject the unpaired Wi-Fi-style connection. The temporary job was then unloaded.
No native pairing support or Finder device entry is enabled by default.
The next transport work is a device-side USB mux/controller bridge; exposing
TCP alone cannot substitute for the initial USB trust/pairing path.

A follow-up loopback and Unix-socket test also failed. On the Unix socket,
stock lockdownd explicitly rejected a non-kernel peer. A userspace TCP-to-Unix
relay therefore does not solve this transport gap. The diagnostic source and
exact observations are recorded in `tools/usb/README.md`; the temporary launch
job was unloaded and is not enabled in default builds.
