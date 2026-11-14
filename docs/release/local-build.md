# Local private package build

On the prepared macOS release checkout:

```bash
./build.sh local --output dist/x1slte-candidate-001
```

This command prepares the pinned iOS 16.5 / 20F66 extraction from Apple's CDN,
checks its content/recipe receipt, generates the loader identity from Apple’s
small device-tree member, locks the prepared inputs, compiles the kernel,
OpenH264, Mesa/AIR and userland, assembles images, packages and verifies the
candidate, and writes:

```text
dist/x1slte-candidate-001/
  boot.img
  recovery.img
  system.apfs
  manifest.json
  SHA256SUMS
dist/x1slte-candidate-001.tar
dist/x1slte-candidate-001.tar.sha256
```

The tar is a private development archive. It has no Odin/TWRP installer backend.
No phone is contacted, flashed, formatted or rebooted. There is no claim of
hardware qualification. Use a new output name for each candidate; existing output
is never overwritten. Logs are in `out/local-build/01-apple.log` through
`05-verify.log`; a failed stage stops the pipeline and prints its log path.
A per-checkout lock rejects concurrent local builds.

## One-time setup

Run the prerequisite check without downloads/building:

```bash
./build.sh local --output dist/x1slte-candidate-001 --check-only
```

This checks tools and board input presence, not their correctness or a successful
build. Provision the host tools described in [building](building.md), including
7-Zip with APFS support, the compiler/linker tools and ldid. The pipeline uses
`xnu-deps/setup.sh` for its SDK/kernel dependencies. At least 25 GiB free is required
before preparing an extraction; budget more for accumulated candidates.

Provide the reviewed inputs under `inputs/x1slte/` and `inputs/tools/`, plus
`inputs/toolchain.json`, the matching HFS kit and the device firmware directories
listed in [building](building.md). Apple supplies the iOS components, not the
Samsung firmware, device-specific UEFI/DT table or optional bootstrap additions.
No personal SSH directory, Wi-Fi credentials, private device config or apps may
be present in this sanitized checkout. A changed board/firmware lock requires
explicit review and `./build.sh lock-inputs`; the local command only automatically
updates the Apple extraction part of an existing lock.

## Reuse and invalidation

- **Apple extraction:** content hashes and recipe hashes must match. A successful
  fetch writes `sysimage/extraction.json`; an old timestamp marker cannot certify
  an extraction. A mismatch reruns preparation, reusing retained DMGs and resumable
  partial downloads. The downloader validates HTTP byte ranges and ZIP CRCs for
  downloaded members. CRC detects corruption; it is not publisher authentication.
- **Kernel:** the existing source-key directory and incremental make build reuse
  unchanged compiled work. This local adapter still runs setup/build checks on
  each invocation; it does not skip the whole kernel stage using a binary cache.
- **Mesa/OpenH264:** pinned source trees and incremental compilation are reused.
- **System APFS:** the private content cache reuses the complete image when its
  actual extraction, firmware, system executables, toolchain and recipe match.
  Changing nested content with identical timestamps still invalidates the key.
  A docs-only commit does not itself require rebuilding the image.
- **Root/recovery/package:** assembled again for the current run and verified.
  Public Actions caches never receive private iOS images. The durable private
  store can deduplicate images across candidate manifests by SHA-256.

A cache hit is an integrity/reuse decision, not evidence that an image boots.
Keep `inputs/`, `.cache/`, `sysimage` downloads and `out/` local and private.

## Validation

The command completed locally and produced `dist/local-validation.tar` on
2026-11-14. Actual extraction and system-image cache hits were observed, and the
packaged system hash matched the cached object. See [validation](validation.md).
