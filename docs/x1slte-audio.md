# Exynos audio recovery and microphone bring-up

The speaker path uses RDMA3, UAIF2 and two CS35L41 amplifiers. Bluetooth's UART fix and unfragmented ACL default are documented in x1slte-bluetooth.md.

## Speaker recovery

A failed amplifier power-up latches playback off. Previously `snd.prep` reset the chips directly from the debug command but left the worker's failure flag set, so successful preparation did not restore automatic playback. It could also race the worker's I2C operations.

`snd.prep` now queues recovery on the audio worker and wakes it from silence. The worker mutes the guard, powers down, resets and prepares both amplifiers, then requires both to respond with no amplifier fault bits before clearing the latch. A reset invalidates the cached volume and presence state. Runtime fault protection remains active; recovery failure leaves playback muted.

Run through the existing `rwsys` helper:

```
rwsys kern.redwood.poke 'snd.prep'
```

The command reports that recovery was queued. Check the kernel log for `snd: amplifier recovery ready` or `FAILED (muted)`; `snd.state` reporting "ready" alone does not establish that playback is active. Do not issue raw `snd.amps` or amp-register writes concurrently with playback.

Validation on 2026-11-07: kernel built and installed; explicit recovery completed with both amps responding; the three-second AudioQueue tone delivered all frames and the worker powered both amps on and then off without reported faults. Listening confirmation is separate from these driver checks.

## Built-in microphone

The Samsung reference kernel has recorded clean voice from the physical mic. Native XNU capture remains experimental: DMIC1 -> VTS serial LIF -> UAIF6 -> NSRC4 -> WDMA4. Earlier capture dumps were regmap cache values, potentially missing configuration written by Samsung's DSP. The bounded reference test enables cache bypass while capture is active to obtain hardware values.

The live `mic.*` commands are diagnostics, not a completed iOS input driver. WDMA movement with all-zero samples does not count as working capture. Reboot clears the live experiments. Preserve the watchdog disable at boot: it addresses the separate bootloader watchdog restart.
