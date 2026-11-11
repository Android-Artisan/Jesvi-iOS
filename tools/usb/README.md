# Native USB mux and Finder

`usb_mux=1` adds a native USB mux interface alongside the existing CDC serial
and Ethernet functions. The device name is Jesvi IOS. Stock macOS usbmuxd and
MobileDevice.framework handle discovery, trust, TLS sessions, and AFC; no Mac
helper daemon is needed for normal use. The on-device lockdownd is socket
activated and listens only on loopback and its native local control socket.

The mux worker connects to native services with actual kernel TCP sockets.
Stock lockdownd checks TCP_PEERPID and classifies those loopback peers as USB.
The IOKit mux/interface services publish actual USB configuration state so
native lockdownd can handle Trust and disconnect notifications. The transport
does not fabricate pairing responses, activation records, or account tokens.

USB interface 4 uses bulk OUT 0x05 and IN 0x85. The research interoperability
mode uses VID/PID 05ac/12a8 for the built-in desktop mobile-device driver match.
Its serial string matches this port's existing synthetic MobileGestalt identity;
it is not an Apple-issued identity or activation credential. The older serial
and ECM-only modes retain their existing descriptors when `usb_mux=0`.

The worker supports sixteen connections, bounded queues, host receive-window
flow control, and split mux frames up to 64 KiB. A single mux packet can span
several 16 KiB USB receive transfers; upstream usbmuxd uses a 48 KiB transmit
MTU. USB disconnect clears queues and closes kernel sockets. All socket I/O is
outside the DWC3 polling path. Native install/check-in services remain subject
to their existing entitlement and pairing requirements.

## Build and diagnostics

The standard x1slte command line enables `usb_mux=1`. Build the kernel and userland normally. The latter includes
`com.redwood.usbmux-lockdown.plist`. Kernel, root APFS image, and loader command
line must all be deployed together. The payload builder checks the final,
resolved command length after root-window substitution.

- `bash tools/usb/check.sh`: host framing/descriptor checks with ASan and UBSan,
  including incomplete and oversized headers and a 48 KiB split frame.
- `python3 tools/usb/mac-list.py`: list public discovery fields through the
  Mac's existing `/var/run/usbmuxd`, without reading pairing records.
- `python3 tools/usb/mac-probe.py DEVICE_ID`: read native device type, display
  name, and OS version over the actual USB connection.
- `bash tools/usb/build.sh`: compile/sign the on-device QueryType diagnostic
  with the locally supplied SDK. `lockdown-probe loopback` is a userspace
  negative control; root `sysctl -n kern.usb_lockdown_query` uses the accepted
  kernel peer path. The Unix socket is a local control channel, not a host
  lockdown stream.

## Verification on SM-G980F, iOS 16.5, 2026-11-11

Native Mac discovery, QueryType, DeviceName=Jesvi IOS, and ProductVersion=16.5
passed. The user saw and approved the native Trust prompt. Apple's native
AMDevicePair, ValidatePairing, StartSession and StartService(com.apple.afc)
returned success. An AFC test created a UUID-named file, wrote/read/verified
its contents, and removed only that file. Pairing remained valid after a USB
link restart. The user confirmed Finder shows Jesvi IOS, iPhone X and storage.
A 256 KiB deterministic-pattern write/read/verify/cleanup then passed after
reboot, with the existing pairing retained. The initial 16 KiB packet limit
was corrected to reassemble 48 KiB host frames. Finder loading remains a
separate UI result: the user confirmed the spinner clears, then clarified that
Finder's management panel reports Activation Failed while the device summary
still shows name, battery and storage. The user chose to keep that real status
visible. Native AFC remains usable; the normal activation-gated Finder panel,
backup, restore and media sync are not claimed. A native application-list query
returned -402653091 while AFC still passed; no compatibility bypass was added.

Native pairing initially failed despite lockdownd holding its required
entitlement. The research AMFI stand-in has no private OSEntitlements object;
CS_OPS_ENTITLEMENTS_BLOB therefore returned success with an empty output. The
kernel now falls back to its existing embedded XML blob, retaining the normal
process/signature checks. On the live daemon, XML output changed from empty
to the existing 5351-byte blob; DER stayed 3488 bytes. No entitlements are
added. Pairing progressed to the normal locked-device/Trust flow and succeeded.

## iCloud

Full native Apple Account sign-in and sync are not established by USB pairing.
The native MAEGetActivationStateWithError read returned `Unactivated`. Existing
activation diagnostics identify unsupported UIK operations in MiniAppleKeyStore
and the absent Secure Enclave-backed activation/device identity. This USB work
does not provide those keys, Apple-issued certificates, or push credentials.
An iCloud Drive location in Files is not evidence that an account is signed in
or synchronizing. No account credentials were requested or used in these tests.

Protocol references:
[usbmuxd device.c](https://github.com/libimobiledevice/usbmuxd/blob/master/src/device.c),
[USB MTU/MRU definitions](https://github.com/libimobiledevice/usbmuxd/blob/master/src/usb.h).
The host daemon socket protocol is a different layer from USB wire framing.
