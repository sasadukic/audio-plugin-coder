# DSP Architecture Specification

## Core Components
- Input conditioning stage (DC-safe, gain-normalized feed into grain engine)
- Circular capture buffer for short-history audio retention
- Grain scheduler (density-driven trigger timing and overlap policy)
- Grain voice engine (windowing, playback pointer interpolation, pitch transposition)
- Cloud mixer (sum/normalize active grains to stable output)
- Output smoothing stage (soft limiting and anti-click handling)

## Processing Chain
Input -> Input Conditioning -> Circular Capture Buffer -> Grain Scheduler -> Grain Voice Engine -> Cloud Mixer -> Output Smoothing -> Output

## Parameter Mapping
| Parameter | Component | Function | Range |
|-----------|-----------|----------|-------|
| Grain Size | Grain Voice Engine | Sets per-grain duration/window length | 1.0 ms to 100.0 ms |
| Density | Grain Scheduler | Controls grain spawn rate and overlap amount | 0.0 to 1.0 |
| Pitch | Grain Voice Engine | Controls transposition of grain playback | -24 to +24 semitones |

## Complexity Assessment
Score: 4/5
Rationale: Real-time granular processing with multiple overlapping grain voices, interpolation, and stable behavior under rapid parameter changes is expert-level DSP work and requires careful performance and artifact control.
