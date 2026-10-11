# PlastiCZ Recovery

An Apple Silicon native **VST3 and Audio Unit preview**, reconstructed from the
installed PlastiCZ 1.1.0 binary using REA/Hopper and original-instruction tests.
VST2 is out of scope at the user's request. The original installed plug-in is
unchanged.

Version **0.3.2** implements the synth, all ten effects, the fixed input filter,
final stereo limiter, and original **4x rendering/downsampling**. Both native plug-in formats match the emulator's
complete C1 renders for all 128 factory presets **bit-for-bit at 44.1 kHz**.
The 38 original parameter slots, factory bank, and legacy preset import remain
intact. The editor now offers **Classic** and **Modern** modes, an explicit named
effect menu, and an active 4x toggle. Every factory preset has 4x off; the enabled
4x path has its own original-instruction regression suite.

Investigation/build date: 2026-10-10, America/Los_Angeles.

## Built plug-ins

- `build/native/PlastiCZRecovery_artefacts/Release/VST3/PlastiCZ Recovery.vst3`
- `build/native/PlastiCZRecovery_artefacts/Release/AU/PlastiCZ Recovery.component`

Both are ARM64 and locally ad-hoc signed. They use a new identity and the name **PlastiCZ Recovery**, so
existing sessions will not automatically replace the original VST2 instance.
The deleted original AU is not required. No system installation is performed by
the build. Once installed in a host's plug-in folder, rescan/restart that host and
load the instrument named PlastiCZ Recovery.

Classic uses the original bitmap artwork, sprite states, and recovered control
positions on an 872×374 front panel. Modern redraws the same arrangement with
cleaner typography and scalable controls, without the rack screws or side strips.
The bottom-right gear reveals the settings row, hidden by default. Classic uses
a smaller, fixed-color pixel gear above the original button lamp; Modern uses
a vector gear. Classic settings use brushed metal, pixel lettering, beveled
buttons and the original 4x lamp. Modern settings use dark rounded panels and
mint accents.
The row switches interface modes and exposes 4x, Import and Panic;
the choice is remembered in host state independently of all sound parameters.
Both interfaces have a named effect selector, effect-specific parameter labels,
FX bypass, 4x, factory-program selection, preset/bank loading and saving, and Panic.
The old Carbon UI is not loaded. Recovery uses a new native JUCE editor.
Opening and closing settings preserves the panel scale. Modern load/save column
headings and preset/bank row labels align with their respective buttons.

After each rebuilt AU/VST3 is signed, CMake refreshes its outer bundle's modified
date. The packaging script preserves this date in the ZIP, including explicit
directory entries, and checks it after macOS extraction. An unchanged/no-op
build retains its previous date. REAPER's startup scan for new/updated plug-ins
must be enabled; use its VST Re-scan command if needed. The folder timestamp
alone is not a guarantee of host cache invalidation.

The output stage reports its lookahead latency to the host (44 samples at
44.1 kHz). Panic clears voices, effects, output and 4x filter histories. Ordinary
4x toggles preserve filter history, as in the original.

## Preset compatibility

`Import preset / bank` accepts the recovered raw chunk format in either byte
order, plus version-1 `FxCk`, `FxBk`, `FPCh` and `FBCh` FXP/FXB containers with the
original `PLCZ` identity. Missing program data is retained from the current bank
when importing. Invalid input is rejected without partially applying it.

Native AU/VST3 state stores all 128 programs, names, the current selection, and
the interface preference. Version 0.3 wraps the unchanged raw bank in a small
versioned header; original raw chunks and Recovery 0.1/0.2 host states still load.
Preset export writes standard FxCk `.fxp` files (38 exact float32 values); bank
export writes FBCh `.fxb` files containing the original raw bank. FXP names have
the standard 28-byte limit; bank files retain all 32 original name bytes.
All 38 parameter slots are preserved, including dummy fields, effects and 4x
settings. The recovered factory bank retains the exact original float32 bits and
32-byte names. The C++ encoder matches the independently extracted 23,564-byte
factory state byte-for-byte.

Real user preset files have not yet been supplied. Version-2 FXB files, old AU
presets, DAW project containers and automatic VST2 session migration are not
implemented. Full-signal bit comparisons currently cover 44.1 kHz; isolated
4x voice/filter comparisons additionally cover 22.05, 48, 96 and 192 kHz.

## All-preset C1 audio regression

The emulator is the accepted sound reference for this project. The complete
native signal path matches **all 128 factory programs, 67,737,600 stereo channel
samples, bit-for-bit**. The actual version 0.3.2 AU and VST3 bundles also match,
tested with the host's tiny-number flushing mode both enabled and disabled:
**270,950,400 channel samples, zero differing bits, zero maximum/RMS error**.
The separate 33,868,800-sample dry regression continues to pass.

This exposed and fixed a wrapper issue: JUCE's default audio guard flushed very
small numbers to zero. The plug-in now uses gradual underflow while processing
and restores the incoming host floating-point state afterward. The previous
largest difference was about 2.07e-36, but exact equality is required by the test.
The effects port also recovered a note-on gate-phase reset: allocating a new
voice resets the pattern clock while mono legato and gate smoothing continue.

Each program receives MIDI note 24 (C1), velocity 100, at 44.1 kHz. The note is
held for two seconds, followed by four seconds of release. A 512-frame no-note
preroll initializes the synth, and 512-frame host blocks retain the fractional
1 ms control clock. Note-off occurs exactly at output frame 88,200. WAV files are
IEEE float32 with no additional normalization or dithering; comparisons check
audio sample words, independently of WAV metadata.

- `analysis/recovered/preset-renders/reference/`: original dry emulator WAVs.
- `analysis/recovered/preset-renders/native/`: native WAVs and comparison CSV.
- `analysis/recovered/preset-renders/difference/`: native minus reference WAVs.
- `analysis/recovered/preset-renders/plugin-comparison.csv`: preserved version
  0.1.0 dry AU/VST3 comparison, with its original build provenance.
- `analysis/recovered/preset-renders/reference-integrity.json`: independent
  reference hashes and parameter/clock validation.
- `analysis/recovered/preset-renders-full/`: separate original stereo references
  with factory effects and final output processing, plus their own manifest.
- `analysis/recovered/preset-renders-full/native/`: exact native full renders,
  per-program comparisons and version 0.2.0 build/test provenance.
- `analysis/recovered/preset-renders-full/plugin-comparison.csv`: actual version
  0.2.0 AU/VST3 comparisons in both incoming floating-point modes.
- `analysis/recovered/preset-renders-full/difference/`: all-zero native-minus-
  original full stereo WAVs.

The full-DSP references preserve every factory parameter. All recovered factory
programs happen to have 4x mode disabled; enabled oversampling is checked by
the separate suite described below. The full reference initialization is validated at 44.1 kHz only; other rates
require a separate lifecycle correction and reference revision.

The full stereo bank is complete: **128 programs, 67,737,600 channel samples**,
with verified factory parameters, file hashes and frozen oracle source copies.
The old dry-preview failure is retained under `preview-comparison/` as historical
evidence; it describes version 0.1.0, not the current effects-enabled build.

Native full regression (no emulator needed once references exist):

```sh
build/native/render_presets --full \
  --reference analysis/recovered/preset-renders-full/reference \
  --output build/native/preset-regression-full/native \
  --difference build/native/preset-regression-full/difference
```

Native dry regression (no emulator needed once references exist):

```sh
build/native/render_presets \
  --reference analysis/recovered/preset-renders/reference \
  --output build/native/preset-regression/native \
  --difference build/native/preset-regression/difference
```

The comparator exits unsuccessfully on even one differing sample bit. A
deliberately flipped reference bit was detected at the exact affected frame.
After reconfiguring CMake with cached references present, CTest includes this
all-program regression. Protocol and reproduction commands are in
`analysis/preset-render-regression.txt` and `analysis/preset-render-oracle-notes.txt`.

## Original 4x mode and interface checks

The original voices run four times per output sample with quarter-sized phase
steps and smoothing factors, followed by the recovered four-section recursive
downsampler. Scratch storage is fixed; no audio-thread allocation is needed.
The original per-call voice cutoff, filter cleanup and retained mode-switch
histories are preserved.

Nineteen original-instruction cases compare **427,817 audio sample words and
183,312 voice/filter state words with zero bit differences**: arbitrary impulses,
noise and tiny inputs; five dry-core sample rates; live threshold switches;
inactive voices; large and irregular block sizes; and full processing with all
11 effect choices. Both actual AU and VST3 bundles also match the full 4x cases
with host subnormal flushing on and off: **1,239,656 channel samples, zero bit
differences**. This is representative 4x coverage, not an all-presets/all-rates
claim. Details: `analysis/oversampling-port-notes.txt`.

The editor checks the settings drawer at three scales, both styles, all effect choices, bypass/4x controls, automation,
all 128 preset displays, exact preservation of non-grid stored values, interface
state restoration, legacy state, and FXP/FXB export/import. Original construction
and drawing evidence is recorded in `analysis/original-ui-notes.txt`.
Current build, reports, source snapshots and executable hashes are preserved in
`analysis/recovered/release-0.3.2/provenance.json`; earlier 0.2 evidence is retained.

## Build and test

Building the AU/VST3 requires macOS, CMake 3.22 or newer, a C++17 toolchain,
and a separate JUCE checkout. This version was built with JUCE 7.0.5,
Xcode 16.2 and the macOS 15.2 SDK. Set the path below to your JUCE checkout.

```sh
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release \
  -DPLASTICZ_JUCE_PATH=/path/to/JUCE
cmake --build build/native -j 4
```

The repository includes the native sources, factory preset data, and Classic
graphics needed to build both plug-ins. The original executable, disassembly,
emulator fixtures, WAV renders, local release snapshots and built bundles stay
outside Git. The original plug-in and emulator are not needed to build or run
the native instrument.

Once the local reference fixtures have been generated, run the complete suite:

```sh
ctest --test-dir build/native --output-on-failure
```

For the dependency-free DSP/state tests, configure another build directory with
`-DPLASTICZ_BUILD_PLUGINS=OFF`. CTest fixture comparisons use the locally recovered
files under `analysis/recovered`; this directory and the original binary are not
published with source. To regenerate them from the user's installed binary:

```sh
python3 scripts/extract_factory.py
python3 scripts/generate_factory_header.py
.venv/bin/python scripts/x86_oracle.py
.venv/bin/python scripts/voice_oracle.py
python3 -m unittest discover -s tests -p 'test_plasticz_state.py'
.venv/bin/python scripts/test_x86_oracle.py
.venv/bin/python scripts/test_voice_oracle.py
```

The local `.venv` pins Unicorn 2.1.4 using
`scripts/requirements-oracle.txt`. Unicorn needs executable JIT memory and
required approved execution outside the Codex sandbox on this machine. Running
the native comparisons against saved fixtures requires no emulator.

The development host loads the built bundles directly, registers the AU only
inside its own process, and does not install it:

```sh
build/native/check_plugin_host_artefacts/Release/check_plugin_host \
  "$PWD/build/native/PlastiCZRecovery_artefacts/Release/VST3/PlastiCZ Recovery.vst3" \
  "$PWD/build/native/PlastiCZRecovery_artefacts/Release/AU/PlastiCZ Recovery.component" \
  --full "$PWD/analysis/recovered/preset-renders-full/reference" \
  "$PWD/build/native/plugin-comparison-full.csv"
build/native/check_plugin_editor --verify "$PWD/build/native/ui-032"
```

Process-local AU registration also requires execution outside this sandbox.
Host smoke-test and editor output are saved in `build/native/`.
The sanitizer/allocation report is retained from 0.3.0 after checking that its
DSP/state inputs are unchanged; 0.3.2 reruns the editor, CTest and actual AU/VST3
audio comparisons.
The reviewed interface images are `build/native/ui-032/editor-classic.png` and
`build/native/ui-032/editor-modern.png`, plus the corresponding `-settings.png`
views. After verification, package with `python3 scripts/package_release.py`;
this refuses unverified binaries and preserves bundle modification dates.

## Verification and its limits

| Check | Result / scope |
|---|---|
| Factory extraction and state | Exact 128 x 38 float values; raw LE/BE codec and FXP/FXB import tested, including malformed input and transactional rejection |
| Waveforms | 35,088 scalar comparisons and all 16,384 cosine-table words match the original-instruction reference exactly |
| Voice core | 1,956 complete 16-voice state snapshots (719,808 words) and 30,720 mono samples match bit-for-bit in six scripted cases |
| Dry C1 regression | All 128 programs and 33,868,800 native dry samples still match the emulator exactly |
| Full stereo C1 regression | All 128 programs, 67,737,600 native samples and 270,950,400 actual AU/VST3 channel samples match the full emulator output bit-for-bit |
| Isolated effects | 44 effect/control/tempo scenarios, 1,940,400 channel samples match original instructions exactly |
| Live effect changes | 22 sequential bypass/type/tempo/note events, 299,376 channel samples match exactly, including retained delay histories |
| Fixed filter/output limiter | 294,948 words match at six sample rates, including asymmetrical peaks, DC, subnormals, zero-length blocks and resets |
| Reverb | 36 standalone original-instruction scenarios match, including fresh and partial reset semantics |
| Full-engine stability | 768 factory/rate/4x cases plus 264 parameter/tempo extremes: 44,630,016 finite channel samples, deterministic prepare/program/panic reset; ASan/UBSan pass |
| Audio-thread allocation | Zero C++ allocations/deallocations in 228,768 guarded processing/MIDI/parameter/panic calls; reviewed DSP calls no direct malloc |
| Block sizes | Maximum observed output difference 0.0000265911 between 127- and 8192-frame blocks across those 384 cases; original per-call near-silent voice cutoff is retained |
| Python reference checks | 12 state-codec tests, five waveform-oracle trust checks and five voice-oracle trust checks pass |
| Native host integration | AU/VST3 load, report 44-sample latency, honor sample-offset MIDI, restore program/parameter state, dispatch all-sound-off and preserve host floating-point mode |
| Editor | Classic/Modern, named FX menu, active bypass/4x, presets/automation/precision/state/export checks; both screenshots visually inspected |
| Original 4x | 19 cases: 427,817 audio and 183,312 state words exact; 1,239,656 actual AU/VST3 channel samples exact |

The oracle runs the original i386 instructions under Unicorn. It does not launch
the old plug-in in a legacy DAW. Imported math functions use this Mac's ARM
libSystem; historical Intel math-library last bits are not established.
Full factory agreement covers one note, velocity, rate and timing protocol.
Isolated stage and live-control tests add coverage but do not establish every
possible performance or parameter combination. Full details are in
`analysis/effects-port-notes.txt` and the individual stage reports.

The native instrument contains no x86 emulator. It reconstructs the recovered
single-precision operation order with fused operations disabled. The original
x87 control word uses truncation for integer waveform lookups; the native code
preserves that behavior. Host MIDI is applied at sample offsets with a persistent
1 ms control clock; exact original event-queue behavior remains unverified.

## Recovered behavior and evidence

The original executable is a universal Mach-O containing **32-bit i386 and
PowerPC**, without x86_64 or ARM64. REA inspected a thin i386 copy after its direct
FAT-binary open selected an unavailable host slice. Hopper reports 3,246
procedures and 1,410 string objects, including SDK/runtime code.

| Recovered subsystem | Original address / evidence |
|---|---|
| Factory bank | Constructor `0x1454a`; `ev_883025d7dfc4d48cbbc678e783d0a948d2d695bb07b138d5560530efa12d92c2` |
| Parameter order | `0x131cc`; `ev_f5e7485630b749115a6c0403301fe67cacc67b8e58e4765f83ccf6c417e1b8b5` |
| Chunk serialization | getChunk `0x13204`, setChunk `0x1356e` |
| Note allocation / legato / release | `0xf5e4`, `0xf3f2` |
| Envelope, pitch, modulation control | `0x10534` through `0x10bb6`; complete tail in `ev_9c0c526910c7c795a0be2d89a0e484bdf9efad8bc95379a15456613f6a3f6bf7` |
| Dry voice renderer | `0xf8d0`; 16 voice records, each `0x5c` bytes |
| Sample-rate scaling | `0x12c9e`; `ev_e50a0f75242d044e1856dd15635887f58ccef4e0fe1ff5d0e896978ad1a4ac5b` |
| Effect dispatcher and quadrature helper | `0x10e4a` through `0x12664`, helper `0x10bbc`; retained REA dossier plus focused original assembly |
| Stereo reverb | Constructor `0x25a8c`, process `0x25ee0`, configure `0x268d6` |
| Fixed input filter and final limiter | Filter `0x24c70`, output `0x2d6aa`; isolated original-instruction fixtures |

| Waveform | Kernel | DC offset |
|---|---|---|
| Saw | `0xef38` | `0xee9a` |
| Square | `0xefbe` | `0xeea2` |
| Pulse | `0xf072` | `0xeeaa` |
| SawPulse | `0xf0fa` | `0xeeb2` |
| DblCos | `0xf182` | `0xeed4` |
| Reso | `0xf20a` | `0xeefe` |
| ResoSqu | `0xf2a0` | `0xef0e` |
| DblSaw | `0xf34a` | `0xeedc` |

Some Hopper pseudocode is incomplete or splits functions, including DblCos and
the control function's tail. The implementation was checked against assembly and
original instructions, not treated as recovered source.

Original executable SHA-256:
`ef52e94d62a8b9988ca9022e8f075f89d5c1cfb838ad3b9eff397788c50f164f`.
Thin i386 SHA-256:
`aee3175bd128a69ee82c8e31a34a36284e21698f022c6f722a6c42f2fc94b635`.
Recovered 23,552-byte factory bank SHA-256:
`763f91576c68a2f15f66db2c41a66644fd1d5e3ab42cf42ebd45ef01df3791f1`.

## Next reconstruction work

1. Extend the all-program C1 regression to pitch-bend/CCs, other notes,
   velocities and sample rates.
2. Extend validation in the user's actual DAWs and test their old preset files.

The effects, final output, original 4x mode, and two interface modes are implemented
for the documented validation scope.

## Saved investigation

- `analysis/evidence/build-phase.rea.json`: 56 evidence records and three tracked
  unresolved questions; canonical export from this build phase.
- `analysis/evidence/preset-render.rea.json`: 58 evidence records and three
  tracked unresolved questions after reference-render investigation. Hopper
  became unresponsive during uncached analysis; its cleanup was not confirmed.
  Focused original assembly and runtime tests supplied the remaining DSP checks.
- `analysis/evidence/build-phase.snapshot.json`: resumable REA snapshot saved
  before closing the active native session.
- `analysis/evidence/plasticz.rea.json` and `plasticz.snapshot.json`: earlier
  feasibility-stage evidence, retained for provenance.
- `analysis/evidence/functions/*.txt`: readable assembly/pseudocode dossiers.
- `analysis/recovered/voice-oracle/`: manifest, instructions, complete voice-state
  fixtures, audio fixtures and native comparison result.
- `analysis/recovered/factory-presets.json`, `parameter-map.json` and
  `waveform-oracle.metadata.json`: exact source data and reference provenance.

To resume REA, open `analysis/source/PlastiCZ-i386` using its absolute path and
`snapshot_path` set to the absolute `build-phase.snapshot.json` path, using the
same Hopper provider. This snapshot retains analysis evidence, not source code.
