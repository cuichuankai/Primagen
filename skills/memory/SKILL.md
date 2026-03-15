---
name: memory
description: Two-layer memory system with grep-based recall.
always: true
---

# Memory

## Structure

- `memory/MEMORY.md` — Long-term facts (preferences, project context, relationships)
- `memory/HISTORY.md` — Append-only event log, search with `grep`

## Usage

Use the `memory` tool to save information:

```
memory(history_entry="[YYYY-MM-DD HH:MM] User is planning a trip to Paris")
```

## Search History

```bash
grep -i "keyword" memory/HISTORY.md
```
