# GameSFXDAW Creative Brief

## Product Hook
GameSFXDAW is a purpose-built audio workstation for game sound design where the core object is an `SFX Container`, not a timeline clip. Each container represents one game event sound and keeps every usable layer and approved version in one place.

## Vision
General DAWs optimize for songs and linear arrangement. GameSFXDAW optimizes for event-based sound authoring, iteration speed, and export to middleware/game engines.

## Primary Users
- Game audio designers building large SFX libraries
- Technical sound designers managing variant sets by platform and gameplay context
- Small game teams that need fast SFX iteration without heavyweight post pipelines

## Core Experience
1. Browse all project SFX in a dedicated `Containers` tab.
2. Open any container to inspect and edit stacked layers.
3. Create named versions for context, quality tier, platform, or gameplay state.
4. Audition, compare, and lock approved versions.
5. Batch export to integration targets (Wwise, FMOD, Unity, Unreal).

## Must-Have Capabilities
- Container-centric browser with categories, tags, status, and ownership.
- Layer stack editing with trim, gain, pitch, timing offset, and per-layer FX chain.
- Version lane with snapshots, notes, A/B comparison, and approval lock.
- Fast preview playback of container output and individual layers.
- Non-destructive editing with history and rollback per container.

## Success Criteria
- User can build one complete SFX asset from raw source files to approved versions without leaving the container view.
- User can locate any project SFX in under 5 seconds via search/filter/tag.
- User can export all approved versions for a project milestone in one batch operation.
