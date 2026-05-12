# Predator RF — Multi-VFO Hold and Hold Decoder Auto-Activation

Read this before touching `core/src/predator/hold_manager.h`,
`core/src/predator/hold_decoder_binder.h`,
`core/src/predator/hold_binding_registry.{h,cpp}`,
`decoder_modules/rtl433_decoder/src/main.cpp`, or the hold wire-up
block in `core/src/gui/main_window.cpp`.

---

## 1. Three nearby concepts that must NOT be conflated

In `main_window.cpp` three things share vocabulary but are unrelated:

- **(a) `predatorHoldOnNewHit` — scan-side hold.** Pauses the scan loop
  when a new hit arrives so the operator can inspect. Nothing to do with
  VFOs.
- **(b) "Marker assignment" — ephemeral per-hit VFO.** Creates a
  `Predator M<n>` VFO tied to a `hits[]` row; dies when the hit ages
  out. Bandwidth-limited to the current SDR window.
- **(c) `predatorHoldManager` — persistent multi-VFO hold list.**
  Entries survive hit pruning AND app restart, each carries its own
  decoder kind, and out-of-band entries are torn down cleanly rather
  than left clipped at the spectrum edge. The "+ Hold" button on hit
  rows pushes a hit's frequency into (c).

When the source retunes such that a held frequency falls outside
`[center − sr/2 + bw/2, center + sr/2 − bw/2]`, the next tick destroys
the VFO; when it comes back in-band, the next tick re-creates it. This
is **not** a round-robin source-retune scheduler — held entries that
span more than the SDR's instantaneous bandwidth are simply marked
"out-of-band" and remain dormant. A future "Multi-band scheduler"
feature would wrap HoldManager with a dwell-weighted retune loop.

**Diagnostic for regression:** `predatorHoldManager.runtimeFor(id).vfo_active`
should match `vfoExists("Predator H" + id)` after every tick. If they
diverge, a callback returned false silently or an exception escaped a
lambda.

The fifth `tick()` parameter `existsCb` is **load-bearing** — without
it, an external teardown of the VFO (decoder module reload, manual
`sigpath::vfoManager.deleteVFO` from another path) would leave
`rt.vfo_active=true` forever and the entry would never re-create. The
wire-up always passes `existsCb`; tests pin both the with-existsCb
(recovers) and without-existsCb (documented stuck state) branches.

Per-entry `decoder` is *persisted and surfaced in the UI* — see
section 2 for how it now drives module activation.

---

## 2. Hold decoder auto-activation (roadmap #5) — RTL433 only in this cut

`core/src/predator/hold_decoder_binder.h` spawns a `core::moduleManager`
instance per held entry whose `decoder` maps to a known SDRPP module.

- Currently only `Native_RTL433 → "rtl433_decoder"`.
- `Native_DSDFME_P25` and the `Radio_*` family are explicitly deferred
  to roadmap #5.5 / #5.6 — `decoderModuleName()` returns `""` for them
  and the binder simply skips.

### 2.1 Three load-bearing pieces

**(a) Two-phase tick.** `preTick` runs **before** `HoldManager.tick` to
tear down any decoder whose VFO is about to disappear (entry
removed/disabled, decoder kind changed, or out-of-band after this
tick) so the bound `dsp::sink::Handler` is detached **before** its
source stream is freed. `postTick` runs **after** to spawn instances
for entries whose VFOs now exist. A single-phase tick would race the
stream destructor and segfault.

**(b) Shared in-band math.** The Binder needs the same in-band math
`HoldManager.tick` is about to apply, so `HoldManager::inBand` is
promoted to `public static` and the binder calls it on the same
`(sourceCenter, sampleRate)` pair the manager will use that frame. If
`HoldManager` ever changes the in-band rule (e.g. adds a hysteresis
margin), the binder needs the same change or it will predict
differently than the manager acts.

**(c) Cross-plugin binding registry.** Decoder modules CANNOT see
`HoldManager` / `Binder` symbols (each is a separate `.so` plugin with
its own private `ConfigManager`), so the binding is passed through
`core/src/predator/hold_binding_registry.h` (process-wide map of
`instance_name → bound_vfo_name`, mirrors the `native_decoder_registry`
pattern).

### 2.2 Sacred call order in the wire-up

- **Spawn:** `setBoundVfoFor → moduleManager.createInstance` — the
  module's ctor reads the binding.
- **Teardown:** `moduleManager.deleteInstance → clearBoundVfoFor` —
  deletion runs the dtor which calls `handler_.stop()`; the binding
  stays alive across that window so a re-spawn under the same name
  doesn't see an empty binding mid-teardown.

### 2.3 rtl433_decoder bound mode

The `rtl433_decoder` ctor inspects `predator::hold::getBoundVfoFor(name_)`;
non-empty → **bound mode**, which:

1. Skips its own `createVFO`.
2. Calls `sigpath::vfoManager.findVFO(boundVfoName_)` to grab the held
   VFO's stream.
3. Skips `deleteVFO` on stop (HoldManager owns the VFO).
4. Forces `enabled_=true` regardless of persisted state — operator
   pause/resume lives on the `HoldEntry`, not on the per-instance
   config.

`findVFO()` was added to `VFOManager` for this. **Do NOT delete the
returned pointer.** Subscribe to `onVfoDelete` if you need
pre-destruction notification (we use `Binder.preTick`'s in-band
prediction instead).

### 2.4 VFO bandwidth handling

Held entries with `decoder=Native_RTL433` get their VFO bandwidth
force-overridden to **250 kHz** (`requiredVfoBandwidth(DecoderKind)`)
at create time because rtl_433 needs a fixed input rate. The
operator-displayed `bandwidth_hz` on the `HoldEntry` is a hint that's
only honoured for non-decoder-bound entries.

### 2.5 Diagnostics

- `predator::hold::boundInstanceCount()` reports live bindings.
- `HoldDecoderBinder::activeCount()` reports spawned instances.
- They should match in steady state. If they diverge, either a
  `setBoundVfoFor` / `clearBoundVfoFor` was missed in the wire-up OR a
  module dtor crashed before reaching `clearBoundVfoFor`.

### 2.6 Test surface

- `tests/hold_decoder_binder_test.cpp` (19 cases / 94 assertions).
- `tests/hold_manager_test.cpp` (141 assertions, includes bandwidth-
  override block).

Beyond the basic spawn / teardown / retry / no-double-spawn /
null-callback coverage the architect insisted on three extra hardening
points that are easy to regress:

**(i) Effective-bandwidth consistency end-to-end.** `HoldManager.tick()`
takes an optional `BandwidthOverrideFn` so `anchorCb` doesn't reset a
bound RTL433 VFO back to the operator's UI bandwidth every frame, and
the binder's `preTick` uses the same `requiredVfoBandwidth(decoder)`
math so its in-band predictions match the manager's keep-alive
decisions. Without this, a narrow UI bw with a wide effective bw would
predict in-band by one and out-of-band by the other → torn-down decoder
still attached to a live VFO.

**(ii) External-instance-delete recovery.** `preTick` and `postTick`
both take an optional `instanceExistsCb` (wire-up passes
`core::moduleManager.instances.find(name) != end()`). When external
delete is detected on an entry that wants to KEEP its instance, binder
silently drops `active_` and `postTick` respawns same frame. If a stale
exists check spuriously erased a still-live entry, `postTick`'s
adoption path catches the resulting `createInstance` collision and
re-tracks the live instance instead of looping forever in
deferred-spawn (mirrors the `existsCb` recovery `HoldManager` itself
has for VFOs).

**(iii) preTick drop-decision must run BEFORE `instanceExistsCb`
short-circuit.** If a false-negative `instanceExistsCb` is allowed to
erase `active_` without firing `destroyCb` when the entry ALSO wants to
be torn down (removed / disabled / out-of-band), `HoldManager` would
destroy the bound VFO while a live `dsp::sink::Handler` is still
attached — the exact race #5 was designed to prevent. `destroyCb` is
therefore called for every drop case **unconditionally** and must be
idempotent (production `moduleManager.deleteInstance` returns -1
silently if name is gone, satisfies contract). Three sub-tests in
`test_preTick_destroyCb_runs_even_when_existsCb_false_negative` pin
entry-removed, entry-disabled, and out-of-band each combined with a
`falseExists` callback.
