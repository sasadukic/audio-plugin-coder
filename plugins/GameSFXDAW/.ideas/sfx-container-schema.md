# SFX Container Data and API Contracts

## 1) Entity Schema (v1)

### Project
```json
{
  "projectId": "proj-atlas",
  "name": "Project Atlas",
  "sampleRate": 48000,
  "bitDepth": 24,
  "lufsTarget": -16.0,
  "containers": []
}
```

### SFXContainer
```json
{
  "containerId": "sfx-footstep-grass",
  "name": "Footstep_Grass",
  "category": "Foley",
  "tags": ["footstep", "outdoor", "player"],
  "status": "in_review",
  "layers": [],
  "versions": [],
  "activeVersionId": "ver-footstep-a",
  "createdAt": "2026-02-25T06:59:35Z",
  "updatedAt": "2026-02-25T06:59:35Z"
}
```

### Layer
```json
{
  "layerId": "lyr-01",
  "name": "Transient",
  "sourcePath": "Assets/Foley/footstep_transient_01.wav",
  "enabled": true,
  "gainDb": 0.0,
  "pitchSemitones": 0.0,
  "startOffsetMs": 0.0,
  "randomPitchCents": 8.0,
  "randomGainDb": 1.5,
  "chancePercent": 100.0,
  "fxChain": ["hp_filter", "transient_shaper"]
}
```

### Version
```json
{
  "versionId": "ver-footstep-a",
  "name": "A_Default",
  "snapshotId": "snap-20260225-001",
  "approved": false,
  "weight": 1.0,
  "notes": "Balanced indoor/outdoor gameplay readability",
  "platform": "all",
  "createdBy": "sound-team",
  "createdAt": "2026-02-25T06:59:35Z"
}
```

## 2) Integrity Rules
- `containerId`, `layerId`, and `versionId` are immutable.
- `Version.snapshotId` must point to a valid immutable snapshot document.
- One container may have at most one `approved == true` version unless explicitly set to multi-approved mode.
- `activeVersionId` must reference an existing version inside the same container.

## 3) Local API Contracts (App Boundary)

### Container API
- `GET /api/containers?category=&search=`: list containers.
- `POST /api/containers`: create container.
- `PATCH /api/containers/{containerId}`: update metadata.
- `DELETE /api/containers/{containerId}`: archive container.

### Layer API
- `POST /api/containers/{containerId}/layers`: add layer.
- `PATCH /api/containers/{containerId}/layers/{layerId}`: update layer processing settings.
- `DELETE /api/containers/{containerId}/layers/{layerId}`: remove layer.
- `POST /api/containers/{containerId}/layers/reorder`: reorder layer stack.

### Version API
- `POST /api/containers/{containerId}/versions`: create version from current layer state.
- `PATCH /api/containers/{containerId}/versions/{versionId}`: update notes/weight/platform.
- `POST /api/containers/{containerId}/versions/{versionId}/approve`: lock approved version.
- `POST /api/containers/{containerId}/versions/compare`: audition A/B pair.

### Export API
- `POST /api/export/render-approved`: render all approved versions.
- `POST /api/export/render-container/{containerId}`: render selected container/version.

## 4) Event Bus Contracts
- `container.selected`
- `container.updated`
- `layer.updated`
- `version.created`
- `version.approved`
- `export.completed`

Each event payload must include:
- `projectId`
- `containerId`
- `entityId`
- `timestamp`
- `changeSet`
