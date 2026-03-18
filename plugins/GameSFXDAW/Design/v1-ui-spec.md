# UI Specification v1

## Layout
- Window: 1440x900 base design (responsive to 1024px width)
- Topbar: product identity, project selector, quick actions
- Tabs: `Containers`, `Timeline`, `Export`
- Containers view:
  - Left panel: category tree + filters
  - Center panel: container cards/list
  - Right panel: inspector with layers + versions

## Sections
1. `Project Sidebar`
2. `Container Browser`
3. `Container Inspector`
4. `Audition and Actions`

## Controls
| Feature | Type | Notes |
|---|---|---|
| Search | Text input | Searches container name and tags |
| Category list | Select list | Filters container browser |
| Container cards | Selectable cards | Shows status, tags, duration, loudness |
| Add Container | Action button | Creates a new draft container |
| Add Layer | Action button | Appends new layer to selected container |
| Add Version | Action button | Snapshots selected container state |
| Approve toggle | Toggle button | Marks version as approved |

## Interaction Rules
- Selecting a container updates inspector instantly.
- Layer edits are local state updates in this prototype.
- Version approval is exclusive by default (one approved version per container).
- Timeline and Export tabs show placeholders for future phases.
