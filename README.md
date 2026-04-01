# Essaim — 32-Voice Rhythmic Oscillator Swarm

A polyphonic rhythmic synthesizer for [Ableton Move](https://www.ableton.com/move/), built on the [Schwung](https://github.com/charlesvestal/schwung) framework.

Essaim features 32 self-triggering voices—one per pad—that respond to the Move's pad grid regardless of layout (chromatic, in-key, fourths, octaves). Each voice is a complete rhythmic oscillator with envelope-driven FM, SVF filtering, and per-sample LFO modulation. Build polyrhythmic textures by pressing pads, then fine-tune individual voice parameters from the Voice menu.

## Key Features

- **32 Independent Rhythmic Oscillators** — one per pad, each with its own speed, decay, timbre, and filter
- **Intelligent Pad Mapping** — all 32 pads respond regardless of Move pad layout (chromatic, in-key, fourths, octaves)
- **Envelope→FM Modulation** — per-voice envelope feeds into frequency for percussion character or slow metallic sweeps
- **Polyphonic SVF Filtering** — each voice has a multimode filter with random SVF mode (LP/BP/HP) and Q
- **6 LFO Shapes** — sine, triangle, soft saw, soft square, skewed sine, warm pulse (all click-free)
- **3 LFO Routing Modes** (Mod Dest) — Volume only, Vol+Filter, or Filter-only modulation
- **Scale Quantization** — 6 scales (Chromatic, Major, Minor, Pentatonic, Whole Tone, Harmonic Minor) with selectable root
- **Frequency/Cutoff Smart Constraint** — when frequency > 70%, cutoff is capped at 80% to prevent harsh highs
- **Smooth Delay with BBD Character** — 2-pole lowpass feedback, clock jitter LFO, soft saturation
- **Hot Saturation Circuit** — quadratic drive curve, volume compensated to 60% max peak
- **Weighted Randomization** — Rnd Patch button creates musically coherent patches via preset ranges
- **24 Named Presets** (+ Init) — start from curated sounds or full randomization

## Operating Guide

### Basic Play

1. **Load Essaim** on a sound generator slot
2. **Press pads** (notes 36–67 by default) to toggle voices on/off
   - Each pad corresponds to one voice and immediately appears in the knob overlay
3. **Pads map flexibly** — press any Move pad and it will trigger the nearest voice, regardless of scale layout
4. **Press a pad again** to deselect it and go back to global controls

### UI Structure

Essaim has **three pages** (navigate with jog wheel):

#### Page 1: Global
Controls that affect the entire patch and all voices.

| Knob | Parameter | Range | Description |
|------|-----------|-------|-------------|
| 1 | Scale | 6 options | Chromatic, Major, Minor, Pentatonic, Whole Tone, Harmonic Minor |
| 2 | Root Note | C–B | Tonal center for scale quantization |
| 3 | Rnd Patch | trigger | Randomize all 32 voices within preset ranges (weighted distribution) |
| 4 | Same Freq | 0–1 | Snap all voices to current voice's frequency (use after pressing a pad) |
| 5 | Init Freq | trigger | Restore all frequencies from last backup (undo Same Freq) |
| 6 | Same Speed | 0.1–40 Hz | Set all voices to same speed (retrigger rate) |
| 7 | Rnd Mod | trigger | Randomize Mod (envelope→FM amount) on all voices |
| 8 | Rnd Pan | trigger | Randomize pan on all voices |

**Menu-only (Global page):**
- **All Mono** — Sum all 32 voices to mono (Off/On)
- **Rnd Voice** — Randomize only the currently-selected voice's parameters
- **Preset** — Select from 25 presets (0=Init random, 1–24=named presets)

#### Page 2: FX
Global effects chain (saturation, delay, master filter).

| Knob | Parameter | Range | Description |
|------|-----------|-------|-------------|
| 1 | Transpose | -3oct to +2oct | Pitch shift for all voices |
| 2 | Fine | ±1 octave | Fine tuning (continuous) |
| 3 | Saturation | 0–100% | Warmth and distortion (quadratic drive, volume-compensated) |
| 4 | Filter | 0–100% | Master DJ filter (0=LP, 50%=bypass, 100%=HP) |
| 5 | Dly Mix | 0–100% | Delay wet/dry (smoothed to prevent clicks) |
| 6 | Dly Rate | 4ms–4s | Delay feedback read position (tape-style pitch warping when turned) |
| 7 | Dly Feed | 0–95% | Delay feedback amount |
| 8 | Dly Tone | 0–100% | Delay tone (0=dark LP, 50%=neutral, 100%=bright + crackle) |

**Menu-only (FX page):**
- **Dly Mode** — Mono, Stereo, Ping-Pong delay feedback routing

#### Page 3: Voice
Per-voice parameters (visible when a pad is pressed).

| Knob | Parameter | Range | Description |
|------|-----------|-------|-------------|
| 1 | Speed | 0.1–40 Hz | Rhythmic retrigger rate (modulated by slow drift LFO) |
| 2 | Mod | ±100% | Envelope→frequency modulation depth (can go negative) |
| 3 | Decay | 5ms–2s | Amplitude envelope decay time |
| 4 | Timbre | 0–100% | Oscillator shape morph (sine → tri → saw → square) |
| 5 | Frequency | 0–100% | Base pitch (quantized to scale, can reach ±1 octave via transposition) |
| 6 | Noise | 0–100% | Noise blend amount (adds texture to osc) |
| 7 | Cutoff | 0–100% | SVF filter cutoff (min 50%, capped at 80% if freq>70%) |
| 8 | Volume | 0–100% | Per-voice output level |

**Menu-only (Voice page):**
- **Attack** — Amplitude envelope attack time (1ms–1s)
- **Pan** — Voice stereo position (–1=left, 0=center, +1=right)
- **Octave** — Voice octave offset (–3 to +2 relative to root note)
- **LFO Shape** — Sine, Triangle, Soft Saw, Soft Square, Skewed Sine, Warm Pulse
- **Mod Dest** — Volume (LFO→vol), Vol+Filt (LFO→vol+cutoff), Filter (LFO→cutoff only)

## Workflow Examples

### Start from a Preset
1. Navigate to Global page
2. Turn knob 8 (Preset menu) and select "Swarm", "Drift", "Pulse", etc.
3. Press pads to enable/disable voices and hear the preset character
4. Adjust Global or FX page to taste

### Create a Custom Patch from Scratch
1. Press the Rnd Patch knob (Global, knob 3) to randomize all voices
   - 50% of voices get volume-only LFO modulation
   - 30% get volume+filter modulation (more dynamic)
   - 20% get filter-only modulation (subtle tremolo)
2. Adjust individual voice parameters by pressing pads and tweaking Voice page
3. Use Transpose (FX page, knob 1) to shift the overall pitch range
4. Add Saturation (FX page, knob 3) for warmth or grit

### Tune All Voices to One Pitch
1. Press a pad to select a voice and hear its pitch
2. Go to Global page, turn knob 4 (Same Freq)
3. All 32 voices now play at that pitch — great for building a unified pad
4. Turn knob 5 (Init Freq) to restore individual voice frequencies

### Create Texture Variations
1. Press a pad and adjust its Voice page parameters:
   - Increase Noisiness for percussive character
   - Increase Cutoff for brightness or decrease for darkness
   - Increase Decay for sustained rings instead of quick taps
2. Use Rnd Voice (Global menu) to randomize just that one voice while keeping others stable

## Technical Details

### Voice Architecture
Each voice runs at audio rate with per-sample processing:
1. **Rhythmic LFO** — driven by Speed knob, modulated by a slow drift LFO (Mod LFO)
2. **Envelope** — triggered by LFO cycle, attack+decay based on attack slider and decay knob
3. **Oscillator** — sine/tri/saw/square morph based on Timbre, blended with noise
4. **SVF Filter** — multimode (randomly LP/BP/HP at voice init), resonant, per-sample cutoff modulation by main LFO
5. **Output** — voice = (filtered osc) × (LFO shape) × (envelope) × (volume)
6. **LFO Modulation** — routes to Volume, Volume+Cutoff, or Cutoff-only depending on Mod Dest

### Frequency Mapping
- Frequency knob (0–1) maps to 48 semitones (4 octaves) quantized to scale
- Scale quantization respects Root Note and selected Scale
- Transposition (FX page) shifts all voices by –3 octaves to +2 octaves
- Fine knob (FX page) adds ±1 octave of fine tuning
- Octave menu (Voice page) offsets individual voice by –3 to +2 octaves relative to root

### Cutoff Constraint
When a voice's Frequency parameter randomizes above 70%:
- Cutoff is capped at 80% max (instead of 95%)
- Prevents harsh, overly-bright high-frequency voices
- Applied in both Rnd Patch and all presets

### Delay Character
- 2-pole cascaded one-pole lowpass in feedback (steeper, warmer rolloff)
- Clock jitter LFO (~0.15 Hz sine, 4% depth) on feedback filter for analog feel
- Soft saturation in feedback path (no overdrive, gentle compression)
- Gate tape noise by signal level (prevents hiss during silence)

### Saturation Curve
- Quadratic drive: `drive = 1 + sat² × 19` (range 1.0 → 20.0)
- Volume compensation: peak normalized to 0.6 (60%)
- At 0% saturation: transparent (drive ≈ 1.0)
- At 100% saturation: aggressive distortion with hard tanh clipping (drive ≈ 20.0)

## Building & Installation

### Build from Source
```bash
./scripts/build.sh
```
Requires Docker or `aarch64-linux-gnu-gcc` cross-compiler (ARM64 for Move).

### Install to Move
```bash
./scripts/install.sh
```
Automatically copies DSP binary and module metadata to Move via SSH, then you must **remove and re-add the module** in Schwung's FX menu to reload.

### Install from Module Store
[Available in the Schwung Module Store](https://github.com/charlesvestal/schwung) on Ableton Move.

## Architecture Highlights

- **Per-sample modulation** — all time-varying parameters (pitch, filter cutoff, pan) are computed per audio sample for seamless sweeps
- **Denormal safety** — all filters initialized and bounds-checked to prevent denormal underflow noise
- **Pad layout agnostic** — pads map to voices 0–31 via modulo arithmetic, detects layout changes via anchor point
- **SameFreq like Weird Dreams** — snaps all voices to current voice's pitch for unified pads
- **Smooth parameter curves** — all knob changes smoothed (5–20ms) to prevent clicks
- **Weighted randomization** — Rnd Patch uses preset ranges instead of uniform random for musically coherent patches

## Credits

Designed and built by [fillioning](https://github.com/fillioning).

Ported DSP patterns from [Weird Dreams](https://github.com/fillioning/weird-dreams-move) and [Octocosme](https://github.com/fillioning/octocosme-move).

## License

MIT — see [LICENSE](LICENSE)

## Version

v0.1.0 — Initial release
