# GameSFXDAW Product Requirements Document (MVP)

## 1. Objective
Deliver a game-audio-first workstation where the primary workflow is authoring SFX containers with internal layers and version sets, then exporting approved outputs to runtime integration targets.

## 2. Problem
Traditional DAWs are timeline-first and force game SFX teams to manage assets, layer variants, and approvals across scattered sessions and spreadsheets.

## 3. MVP Scope
- Dedicated `Containers` tab with category tree, search, and metadata filters.
- Container inspector with editable layer stack.
- Version lane with snapshot creation and approval lock.
- Per-container audition controls and A/B comparison.
- Batch export of approved versions.

Out of scope for MVP:
- Full music composition timeline replacement.
- Real-time collaborative editing.
- Cloud sync and permissions.

## 4. User Stories
- As a sound designer, I can open one container and see all layers that define that SFX.
- As a lead, I can mark a version as approved and lock it for milestone export.
- As an integrator, I can export approved containers in one pass with naming conventions.

## 5. Functional Requirements
- FR-01: System stores all SFX as containers with unique IDs.
- FR-02: Container supports multiple layers with independent processing controls.
- FR-03: User can create named versions from current layer states.
- FR-04: User can audition versions A/B without leaving the inspector.
- FR-05: User can filter and search containers by tag, category, status, and text.
- FR-06: Export pipeline can render one container or all approved containers.

## 6. Non-Functional Requirements
- NFR-01: Container list interaction remains responsive under 1000 containers.
- NFR-02: Preview latency target under 20 ms in local monitoring context.
- NFR-03: Autosave every 10 seconds with crash-safe restore points.
- NFR-04: Deterministic render output for same snapshot and render preset.

## 7. Success Metrics
- Time to create first playable container from raw layers: under 3 minutes.
- Time to locate and update existing SFX: under 30 seconds median.
- Export failure rate for approved batch: under 1%.

## 8. Delivery Milestones
1. Domain model and persistence.
2. Containers tab and inspector UI.
3. Audition engine and version snapshoting.
4. Batch export and middleware profile presets.
