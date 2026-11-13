# Validation record — 2026-11-14

Worktree: `work/hackios-release-devops`, branch `release/exynos990-devops`, based on
`e3589a1cade9bfe06d0438d805147e33b18de8d7`. The active `x1slte` checkout and its
untracked files were preserved. No phone connection, flash, reboot, push, remote
setting change or GitHub workflow dispatch was performed.

## Passed locally

- Fresh pinned XNU materialization: `xnu/apply.sh` applied 84 changed files and
  288 overlay files. First audit build used the local Apple-source clone only as a
  read-only Git source; the release adapter separately materialized its own source
  tree from the pinned remote. Neither borrowed compiled kernel output.
- Native dependency setup and **source kernel compilation**, including the new
  `./build.sh kernel --device x1slte --jobs 4` adapter. Stripped output was
  **15,144,376 bytes**, below the **16,777,216-byte** loader limit. Compiler: Apple
  clang 21.0.0 on the supplied Apple Silicon macOS host. Build metadata/checksum are
  written to ignored `out/kernel/`; no firmware or iOS system image was built.
- **27 Python tests** (`bash ci/check.sh`): nested content changes with unchanged
  mtimes; copy/location and mtime stability; mode/options/add/remove invalidation;
  checksum corruption; failed atomic publication; concurrent cache writers;
  missing inputs; output symlinks; source-only edits preserving the system key;
  firmware/provider changes invalidating it; symlink firmware rejection; generated
  tool files not invalidating source inputs; Android v2/Samsung table validation;
  truncated APFS/boot images; HFS CRC/capacity; exact model/bootloader/geometry/mount
  evidence; unsupported targets; protected destination rejection; full synthetic
  candidate verification/tampering; boot/recovery mismatch; AVB overlap; private
  store deduplication across candidate metadata changes.
- Shell syntax for new entry points, CI helper and edited legacy build scripts.
- Python syntax for every release helper.
- `actionlint` **1.7.12** passed all four new workflows; custom runner label is
  declared in `.github/actionlint.yaml`.
- `git diff --check` and current release-document local link checks.
- Negative CLI checks: y2s build refuses an unmaintained board; image doctor refuses
  missing private inputs. These failures are expected and protective.
- Gitleaks **8.30.1**, with redaction and `--all --full-history`, scanned local Git
  refs. Four generic-api-key findings were inspected: two SIMD local-variable
  expressions (`data_key_lo` / `data_key_hi`) repeated in historical zstd source
  commits. They are false positives, not credential literals. Redacted reports
  remain ignored in `out/`. The new release helper directory also passed a scan.
  This does not certify absence of personal identifiers, firmware or license issues.

## Not established

- New workflows have not run on GitHub; hosted macOS toolchain differences remain
  to test. Private worker and storage volumes have not been provisioned.
- A fully clean, all-source userland/UEFI/complete image build has not been run.
  The dedicated worktree intentionally lacks the owner's private extraction,
  firmware, DT table and UEFI input snapshot; no files were borrowed from the other
  agent's active build tree to hide those missing prerequisites.
- A synthetic APFS/header fixture validates safety logic, not filesystem integrity
  or bootability. Complete APFS checking and actual AVB signature validation are
  still required for a real candidate.
- No first-install/update/rollback procedure has been tested on hardware. The host
  helper remains read-only; there is no qualified TWRP ZIP or Odin release package.
- Publication inventory/provenance review remains open: the initial tracked-tree
  audit found 164 review items, including 154 binaries/artifacts and 42
  research/prebuilt items (categories overlap). A secret-pattern scan cannot clear
  their provenance or redistribution status.

Use the [release checklist](checklist.md) to close these gaps. Do not turn a passing
kernel build or these host tests into a claim that the release is ready to flash.
