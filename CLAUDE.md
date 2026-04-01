# Essaim — Claude Code context

## What this is
32-voice rhythmic oscillator swarm for Ableton Move.
Schwung sound generator. API: plugin_api_v2_t. Language: C.
Voice architecture: 32-voice, one per pad (notes 36–67), toggle on/off, self-triggering.

## Repo structure
- `src/dsp/essaim.c` — all DSP logic (32 voices, SVF, envelopes, scale quantization)
- `src/module.json` — module metadata and version (must match git tag on release)
- `scripts/build.sh` — Docker ARM64 cross-compile (always use this)
- `scripts/install.sh` — deploys to Move via scp + fixes ownership
- `.github/workflows/release.yml` — CI: verifies version, builds, releases, updates release.json

## Parameter pages (3 pages, jog-wheel navigation)
- **Global**: Scale, Root, Rnd Patch, Rnd Voice, SameFreq, SameSpeed, InitFreq, Rnd Pan (+All Mono)
- **FX**: Transpose, Fine (±1oct continuous), Saturation, Filter, Dly Mix, Dly Rate, Dly Feed, Dly Tone (+Dly Mode menu-only)
- **Voice** (dynamic — reflects most-recently-pressed pad):
  Speed (retrigger Hz), Mod (env→FM), Decay (envelope), Timbre (shape morph),
  Frequency (pitch, quantized), Noisiness (noise blend), Cutoff (SVF), Volume

## Voice architecture
- 32 independent self-triggering rhythmic oscillators
- Each pad (notes 36–67) toggles its voice on/off
- Pressing a pad also switches knob overlay to that voice
- Each voice has randomized SVF mode (LP/BP/HP) and Q — not user-controllable
- Each voice starts with slightly randomized defaults (speed, timbre, frequency, etc.)
- Oscillator shape morphs: sine → triangle → saw → square (timbre param)
- Scale quantization: frequency param maps to 48-semitone range, snapped to scale degrees

## Critical constraints
- NEVER write to `/tmp` — use `/data/UserData/` on device
- NEVER allocate memory in `render_block` — all state lives in the instance struct
- NEVER call printf/log/mutex in `render_block`
- Output path: `modules/sound_generators/essaim/` (not audio_fx!)
- Files on Move must be owned by `ableton:users` — `scripts/install.sh` handles this
- `release.json` is auto-updated by CI — never edit manually
- Git tag `vX.Y.Z` must match `version` in `src/module.json` exactly
- `get_param` MUST return -1 for unknown keys (not 0)
- CPU budget: 32 voices × (osc + SVF + env + noise) per sample — keep DSP lean

## Build & deploy
```bash
./scripts/build.sh          # Docker ARM64 cross-compile
./scripts/install.sh        # Deploy to move.local
```

## Release
Use the `/move-schwung-release` skill.

## License
MIT
