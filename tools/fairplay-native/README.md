# Native FairPlay prototype

Branch: `feat/s20-native-fairplay`, separate worktree `work/hackios-fairplay-native`.
This is unfinished interoperability work. Native Store downloads are not fixed.
No firmware, credentials, captured account data, or generated native modules belong
in Git. Keep those in the workspace's ignored `out/fairplayed-audit/private/`.

Changes visible in Source Control:

- `xnuloader/loader.c` and `native_sidecar.h`: optional bounded module-payload
  loader; default boot behavior has no sidecar operation.
- `xnu/redwood.patch`: native module admission only with `fairplay_native=1`.
  The edited source is in this worktree's ignored `xnu-src/.../libsa/bootstrap.cpp`,
  exported through the normal XNU workflow. The shared main checkout is unchanged.
- This directory: inventory, reconstruction, emulation, packing and tests.
- `docs/fairplay-native.md`: evidence, limitations and remaining steps.

Run host tests from this worktree:

```
python3 -m unittest discover -s tools/fairplay-native -p 'test_*.py' -v
```

`emulate_initializers.py` requires Unicorn; `audit_flow.py` requires Capstone.
These are offline tools. The C validator tests use the local Clang compiler and
synthetic payloads, never execute Apple instructions, and perform no phone access.

All generated image manifests currently say DO_NOT_DEPLOY. The first full-layout
prototype exceeds the existing kernel read window. `sidecar.py` produces a compact
kernel within that window plus a separate native payload. The new loader expects
payload entries `fpart=recovery`, `foff=1800000`, `fsize=<hex actual size>` and
padded storage for the sidecar at recovery+24 MiB; packaging and live verification
remain unfinished. Kernel/loader sources must be rebuilt together and native
imports rebound to the exact rebuilt kernel before any test.
