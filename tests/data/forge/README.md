# .forge fixtures

Files written by earlier format versions. **Never regenerate them**: they stand
for files users already have, and every later build must keep loading them
through the migration chain (`f3d::io::forge_migrations()`). When the format
changes, add a new fixture for the new version next to the old ones.

| File | Format | Written by | Contents |
|---|---|---|---|
| `v1_basic.forge` | 1 | forge3d 0.1.0 (P2.6) | `forge3d-cli --quiet run tests/data/scenes/outliner_layers.json save tests/data/forge/v1_basic.forge` (`.gitattributes` treats `*.forge` as binary). 7 entities in 3 roots (Props → Rock A, Rock B at (2, 0, 1); Crates → Crate 1; Lights → Sun) with fixed GUIDs `00000000-0000-4000-8000-00000000000N`. 3 layers: Environment (locked, holds Props), Hidden props (hidden, holds Crates), Lighting (holds Lights). Bookmark "Overview". No editor.json, no binary chunks. |

`tests/unit/io/forge_file_test.cpp` ("the checked-in v1 fixture loads") checks these contents.
