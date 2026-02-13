# UI Specification v1

## Layout
- Window: 720x420px
- Sections: Header, Main Controls, Readout Strip, Footer Status
- Grid: 3-column main control grid with one rotary control per parameter

## Controls
| Parameter | Type | Position | Range | Default |
|-----------|------|----------|-------|---------|
| grain_size | Rotary Knob | Main Grid, Column 1 | 1.0 - 100.0 ms | 35.0 ms |
| density | Rotary Knob | Main Grid, Column 2 | 0.0 - 1.0 | 0.55 |
| pitch | Rotary Knob | Main Grid, Column 3 | -24 to +24 semitones | 0.0 st |

## Color Palette
- Background: #111115
- Panel Surface: #1E1E24
- Primary: #00FFFF
- Secondary Accent: #FF0099
- Text Primary: #F0F0F5
- Text Secondary: #A0A0B0
- Border: #333340

## Style Notes
- Adopt NeoGrid Minimal visual language: sharp geometry, subtle neon glow, and dark panel hierarchy.
- Keep labels uppercase and compact for hardware-style readability.
- Use high-contrast value readouts under each knob for quick live tweaking.
