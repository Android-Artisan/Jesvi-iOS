# Native Finder transport investigation

This directory contains a diagnostic, not a Finder implementation. The current
gadget has serial and Ethernet interfaces; it has no USB mux interface. Changing
the USB product name does not add Finder pairing or file sharing.

Build the probe with `bash tools/usb/build.sh`. It uses the locally supplied
iPhoneOS SDK and signs only our executable. The executable sends one `QueryType`
request and reads at most 16 KiB. It does not request pairing, change trust,
install software, or copy pairing credentials. Socket reads/writes time out
after five seconds. Use `lockdown-probe unix`, `loopback`, or `usb` on the phone;
the respective destinations are `/var/run/lockdown.sock`, `127.0.0.1:62078`, and
`10.77.0.2:62078`. A listener must already exist.

## Live results, 2026-11-11

The stock iOS 16.5 lockdownd was temporarily loaded from a diagnostic launch
recipe, with no keepalive or panic-on-crash policy. The listener was restricted
to the USB address, then loopback for the local test. The Unix socket was also
tested. Each temporary job was unloaded afterward.

- The TCP request was reset. Native log: `socket_accept_callback: Killing the
  WiFi connection immediately.`
- The Unix socket closed the request. Native log: `peer_is_safe: non kernel
  peer, pid 380!`, followed by `socket_accept_callback: Peer is unsafe.`
- Native log also reports `we failed to create an IOUSBDeviceController`.

The Unix socket therefore cannot be used as a userspace proxy for initial USB
pairing. The TCP listener also does not establish initial USB trust. Do not
enable lockdownd in the default job list on the strength of these probes.

The next implementation requires a device-side USB mux transport and the
kernel connection path accepted by stock lockdownd. It must retain the existing
serial/Ethernet interfaces for recovery, handle USB disconnect/reconnect, and
use the native trust/pairing flow. Actual Finder sidebar, trust prompt, eject,
and AFC file-sharing tests are still pending.

The host USB wire framing reference is upstream
[usbmuxd device.c](https://github.com/libimobiledevice/usbmuxd/blob/master/src/device.c).
Its host daemon protocol is a different layer and cannot simply be placed on
a new USB bulk endpoint.
