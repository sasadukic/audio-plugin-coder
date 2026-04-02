# DSP and Product Architecture Specification

## Core Components
- `ProjectGraph`: owns containers, tag indices, undo stack, and project metadata.
- `ContainerEngine`: resolves active container, layer ordering, and render routing.
- `LayerProcessor`: per-layer trim, gain, pitch, randomization, and optional effect chain.
- `VersionManager`: immutable snapshots and approval lifecycle for versions.
- `AuditionBus`: low-latency preview path for layer solo, container mix, and A/B compare.
- `ExportService`: batch rendering and profile-based export for middleware targets.
- `AssetStore`: source file registry, waveform metadata, and offline peak/loudness cache.

## Processing Chain
For real-time preview of one container:

`Input Source Layer(s) -> Trim/Offset -> Gain/Pitch -> Layer FX -> Container Bus -> Version Delta -> Master Preview Bus -> Output`

For export:

`Container Snapshot -> Offline Render Graph -> Loudness Normalization -> File Naming/Profile Rules -> Delivery Folder`

## Parameter Mapping
| Parameter | Component | Function | Range |
|-----------|-----------|----------|-------|
| `layer_gain_db` | LayerProcessor | Per-layer static gain | -36.0 to +12.0 dB |
| `layer_pitch_semitones` | LayerProcessor | Pitch shift per layer | -24.0 to +24.0 st |
| `layer_start_offset_ms` | LayerProcessor | Layer launch delay | 0.0 to 500.0 ms |
| `container_output_gain_db` | ContainerEngine | Post-layer mix gain | -24.0 to +12.0 dB |
| `version_weight` | VersionManager | Weighted selection at runtime | 0.0 to 1.0 |
| `project_lufs_target` | ExportService | Target loudness normalization | -30.0 to -10.0 LUFS |

## Data Model
- `Project` has many `SFXContainer`.
- `SFXContainer` has many `Layer` and many `Version`.
- `Version` stores references to layer states (snapshot IDs) and render metadata.
- `LayerState` is immutable once snapshotted to ensure reproducibility.

## Complexity Assessment
Score: `4 / 5`

Rationale:
- Multi-entity state model with snapshot semantics and approval workflow.
- Real-time audition path plus offline export path.
- Layer randomization and versioning rules increase state and QA complexity.
- Requires strong UI information architecture beyond standard plugin control layouts.
