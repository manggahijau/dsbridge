# dsbridge

Bridges the 32-bit-only Dolby `libdseffect.so` ("DS1"/"dsplus" effect) into a
64-bit-only `audioserver` process, for devices where the framework audio
process is arm64 but the vendor Dolby effect binary was only ever built for
armeabi-v7a.

**Read the whole README before flashing anything. A bug here can crash
`audioserver`, which takes down ALL audio on the device (calls, media,
notifications), not just the Dolby app.**

## How it works

```
audioserver (64-bit)                 dsbridged (32-bit, standalone daemon)
  |                                      |
  |  dlopen("libdsbridge.so")            |  dlopen("libdseffect.so")  <-- the real Dolby binary
  |  (our shim, registered in            |
  |   audio_effects.xml in place         |
  |   of the original libdseffect.so)    |
  |                                      |
  |  create_effect() / process() /       |
  |  command()  ------ Unix socket ----->|  EffectCreate() / (*handle)->process() / ->command()
  |  (only scalars + copied PCM bytes,   |  on the REAL vendor binary
  |   never raw pointers, cross here)    |
```

`dsbridged` must run as its own long-lived process (started from the
module's `service.sh` at boot). It must **never** be `fork()`'d from inside
`audioserver` -- forking a complex multi-threaded daemon like audioserver is
a great way to hang or crash the whole system.

## Building (GitHub Actions, no local NDK needed)

1. Create a new GitHub repo and push everything in this folder to it
   (including `.github/workflows/build.yml`).
2. GitHub Actions runs automatically on push (or trigger manually via the
   "Run workflow" button under the Actions tab).
3. Once the run finishes, open the run, scroll to **Artifacts**, and
   download:
   - `dsbridged-armeabi-v7a` -> contains the file `dsbridged`
   - `libdsbridge.so-arm64-v8a` -> contains the file `libdsbridge.so`

## Installing into the Magisk/KernelSU module

Copy the two build outputs into the module like this:

```
<module root>/dsbridged                                  (chmod 755)
<module root>/system/vendor/lib64/soundfx/libdsbridge.so
```

The module already ships:
- `system/vendor/etc/audio_effects.xml` -- a copy of the device's own stock
  config with only the `ds` library's `path` changed from
  `libdseffect.so` to `libdsbridge.so`. Everything else (all UUIDs, all
  other libraries) is untouched.
- `service.sh` -- starts `dsbridged` at boot, pointed at the original
  32-bit `libdseffect.so` shipped elsewhere in the module.
- `sepolicy.rule` -- lets `audioserver` connect to `dsbridged`'s socket.

## Staged testing (do this in order, do not skip steps)

### Stage 0 -- safety net
Before testing, know how you'll recover if `audioserver` crash-loops (no
sound device-wide): have a way to quickly disable/remove the module (recovery,
fastboot, or a second working root shell session) ready. If a reboot loop or
audio deadlock happens, disable the module and reboot.

### Stage 1 -- daemon starts and loads the real library
```
logcat -c
# reboot or manually run: /data/adb/modules/dsplus/dsbridged /data/adb/modules/dsplus/system/vendor/lib/soundfx/libdseffect.so &
logcat -d | grep dsbridged
```
Expect: `loaded ... ok, EffectCreate=0x... EffectRelease=0x... EffectGetDescriptor=0x...`
If you see `dlopen failed` or `missing required symbols`, stop here --
something about the 32-bit build environment is different from what was
assumed; do not proceed to Stage 2.

### Stage 2 -- shim loads inside audioserver (no user interaction yet)
Restart audioserver (or reboot) with the new `audio_effects.xml` +
`libdsbridge.so` in place, then:
```
logcat -d | grep -i "EffectsFactory\|libdsbridge\|audioserver"
```
Expect no crash of `audioserver` (check `pidof audioserver` didn't change /
restart). If audioserver restarts repeatedly, **disable the module
immediately** (Stage 0) -- this means the shim's exported symbol or ABI is
wrong and is crashing the effects factory at startup/scan time.

### Stage 3 -- descriptor + create/release only
Open the Dolby app just enough to trigger `DsService.createDs()` once, then:
```
logcat -d | grep -iE "dsbridged|libdsbridge|DsService|AudioEffect"
```
Expect `dsbridged` log lines `created instance id=0 ...` and no native crash.
If `EffectCreate failed`, the UUID/descriptor path has an issue -- check the
`EffectGetDescriptor`/`EffectCreate` return codes logged by `dsbridged`.

### Stage 4 -- SET_CONFIG
Expect a `dsbridged` log line `SET_CONFIG rate_in=... ch_in=... -> rc=0`.
If `wire.inputCfg.hadProvider` or `outputCfg.hadProvider` warnings appear in
`dsbridged`'s log ("SET_CONFIG requested EFFECT_CONFIG_PROVIDER..."), the
buffer_provider assumption in this bridge is wrong for this effect and audio
will be silent/broken until that's implemented -- stop and report back.

### Stage 5 -- actual audio processing
Only after Stages 1-4 are clean, play music with the effect toggled on/off
and listen for the actual Dolby processing. Expect possible glitches/latency
under this Phase-1 (blocking socket, no shared memory) implementation --
that's a known limitation, not necessarily a bug, see below.

## Known limitations / likely next steps

- **Phase 1 IPC is a plain blocking socket round-trip per audio buffer.**
  This is correctness-first, not latency-optimized. If Stage 5 works but
  glitches under load, the next step is shared memory + realtime scheduling
  for the daemon thread (matching audioserver's audio thread priority),
  not a redesign.
- **`buffer_provider_t` callbacks are not implemented.** If `dsbridged` logs
  the "hadProvider" warning, this bridge does not support it yet.
- **44.1kHz lock-in**: community notes on this exact Dolby binary say
  `SET_CONFIG` to a different sample rate doesn't really take effect inside
  the vendor binary itself. If your pipeline runs 48kHz, you may need a
  resampler in `dsbridged` around the `process()` call. Not implemented here
  yet -- add it if Stage 5 produces audio at the wrong pitch/speed.
- **Generic command passthrough assumes POD payloads.** Only
  `EFFECT_CMD_SET_CONFIG` is marshaled field-by-field. If some other Dolby
  proprietary command code turns out to carry pointers, it needs the same
  treatment `EFFECT_CMD_SET_CONFIG` gets in this code.
