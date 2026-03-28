# Essaim

32-voice rhythmic oscillator swarm for [Ableton Move](https://www.ableton.com/move/),
built for the [Schwung](https://github.com/charlesvestal/schwung) framework.

## Features

- 32 independent self-triggering rhythmic oscillators — one per pad
- Toggle pads on/off to build polyrhythmic textures
- Each voice unique on load: randomized SVF mode, Q, pan, and initial parameters
- Oscillator shape morphing (sine → triangle → saw → square)
- Scale quantization with selectable root note and scale
- Envelope → frequency modulation for percussive FM character

## Controls

| Control | Function |
|---------|----------|
| Pads | Toggle voice on/off + select for knob editing |
| Jog wheel | Browse between Global and Voice pages |
| Knobs 1-8 | Adjust parameters for current page/voice |

## Parameters

### Global
| Knob | Parameter | Description |
|------|-----------|-------------|
| 1 | Root Note | Tonal center (C through B) |
| 2 | Scale | Chromatic, Major, Minor, Pentatonic, Whole Tone, Harmonic Minor |

### Voice (per-pad, shown when pad is pressed)
| Knob | Parameter | Description |
|------|-----------|-------------|
| 1 | Speed | Rhythmic retrigger rate (0.1–40 Hz) |
| 2 | Mod | Envelope → frequency modulation depth |
| 3 | Decay | Amplitude envelope decay (5ms–2s) |
| 4 | Timbre | Oscillator shape morph |
| 5 | Frequency | Pitch (quantized to scale) |
| 6 | Noisiness | Noise blend amount |
| 7 | Cutoff | SVF filter cutoff |
| 8 | Volume | Per-voice output level |

## Building

```
./scripts/build.sh
```

Requires Docker or an `aarch64-linux-gnu-gcc` cross-compiler.

## Installation

```
./scripts/install.sh
```

Or install via the Module Store in Schwung.

## Credits

Designed and built by fillioning.

## License

MIT — see [LICENSE](LICENSE)
