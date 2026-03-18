# GameSFXDAW Parameter Spec

This parameter set defines the first implementation target for container-based SFX authoring.

| ID | Name | Type | Range | Default | Unit | Scope |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `master_preview_gain_db` | Master Preview Gain | Float | -24.0 to +12.0 | 0.0 | dB | Project |
| `project_lufs_target` | Loudness Target | Float | -30.0 to -10.0 | -16.0 | LUFS | Project |
| `container_output_gain_db` | Container Output Gain | Float | -24.0 to +12.0 | 0.0 | dB | Container |
| `container_random_seed` | Variation Seed | Int | 0 to 65535 | 0 | none | Container |
| `layer_gain_db` | Layer Gain | Float | -36.0 to +12.0 | 0.0 | dB | Layer |
| `layer_pitch_semitones` | Layer Pitch | Float | -24.0 to +24.0 | 0.0 | st | Layer |
| `layer_start_offset_ms` | Layer Start Offset | Float | 0.0 to 500.0 | 0.0 | ms | Layer |
| `layer_random_pitch_cents` | Layer Random Pitch | Float | 0.0 to 100.0 | 0.0 | cents | Layer |
| `layer_random_gain_db` | Layer Random Gain | Float | 0.0 to 12.0 | 0.0 | dB | Layer |
| `layer_chance_percent` | Layer Play Chance | Float | 0.0 to 100.0 | 100.0 | % | Layer |
| `version_weight` | Version Weight | Float | 0.0 to 1.0 | 1.0 | none | Version |
| `version_ducking_db` | Version Ducking | Float | -18.0 to 0.0 | 0.0 | dB | Version |
| `tail_hold_ms` | Tail Render Hold | Float | 0.0 to 4000.0 | 250.0 | ms | Render |
| `crossfade_ms` | Version Crossfade | Float | 0.0 to 500.0 | 25.0 | ms | Audition |

## Notes
- Project and container parameters are automation-capable but stored as metadata snapshots.
- Layer parameters are the primary authoring controls and are versioned through snapshot references.
- Version parameters are lightweight deltas over a container baseline to reduce duplicate state.
