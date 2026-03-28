# Essaim — Design Specification

## Overview
**Essaim** (French: swarm) — 32-voice rhythmic oscillator swarm for Ableton Move.
Each of the 32 pads activates an independent self-triggering rhythmic oscillator.
Voices layer into polyrhythmic textures. Each voice is unique on load:
randomized SVF mode (LP/BP/HP) and Q, slight parameter drift.

## Architecture
- **Component type:** sound_generator
- **Voices:** 32 (one per pad, notes 36–67)
- **Trigger model:** Toggle — first press activates rhythmic pulsing, second press silences
- **Pad → knob focus:** Pressing a pad switches the 8 knobs to that voice's parameters
- **Self-triggering:** Each voice has an internal clock at `speed` rate; on each cycle the decay envelope retriggers

## Per-Voice Parameters (8 knobs, shown when pad selects voice)
| Knob | Param     | Range        | Description |
|------|-----------|--------------|-------------|
| 1    | Speed     | 0.1–40 Hz    | Rhythmic retrigger rate |
| 2    | Mod       | 0–1          | Envelope → frequency modulation depth |
| 3    | Decay     | 5ms–2s       | Amplitude envelope decay time |
| 4    | Timbre    | 0–1          | Oscillator shape morph: sine→tri→saw→square |
| 5    | Frequency | 0–1          | Pitch (quantized to Root Note + Scale) |
| 6    | Noisiness | 0–1          | Noise blend (equal-power crossfade) |
| 7    | Cutoff    | 0–1          | SVF filter cutoff (30–18000 Hz, squared mapping) |
| 8    | Volume    | 0–1          | Per-voice output level |

## Hidden/Random Per-Voice (set on create_instance, not user-controllable)
- **SVF mode:** random LP (0), BP (1), or HP (2)
- **SVF Q:** random 0.3–0.95
- **Initial param scatter:** each voice starts with slightly randomized defaults

## Global Parameters (Global page via jog wheel)
| Knob | Param      | Range     | Description |
|------|------------|-----------|-------------|
| 1    | Root Note  | 0–11 enum | C, C#, D, D#, E, F, F#, G, G#, A, A#, B |
| 2    | Scale      | 0–5 enum  | Chromatic, Major, Minor, Pentatonic, Whole Tone, Harmonic Minor |
| 3–8  | —          | —         | Reserved |

## UI Hierarchy
```
Root → Global (Root Note, Scale)
     → Voice  (Speed, Mod, Decay, Timbre, Frequency, Noisiness, Cutoff, Volume)
```
Voice page knobs dynamically reflect the most-recently-pressed pad's voice.

## DSP Details
- **Oscillator:** Phase accumulator with shape morphing (timbre param)
  - 0.0 = sine, 0.33 = triangle, 0.66 = saw, 1.0 = square
- **Envelope:** Instant attack, exponential decay (decay param)
- **Filter:** State-variable filter (SVF), mode/Q fixed per voice at load
- **Noise:** Xorshift white noise, equal-power crossfaded with oscillator
- **Mod:** Envelope output modulates oscillator frequency (FM percussion character)
- **Output:** All 32 voices summed, soft-clipped via tanh, stereo (slight random pan per voice)
- **Scale quantization:** Frequency param (0–1) maps to ±24 semitones from root, snapped to scale
