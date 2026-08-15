# KeePassXC interoperability fixtures

These binary fixtures are copied without modification from the official
KeePassXC 2.7.12 source archive (`tests/data/`). The archive SHA-256 is pinned
in `third_party/dependencies.lock.json`.

| File | Purpose | Upstream test password |
| --- | --- | --- |
| `Format300.kdbx` | KDBX 3 header/read compatibility | `a` |
| `Format400.kdbx` | KDBX 4 header/read compatibility | `t` |
| `ProtectedStrings.kdbx` | Protected-field compatibility | `masterpw` |
| `BrokenHeaderHash.kdbx` | Authenticated-header rejection | empty |

Fixture credentials are public test data and must never be reused as product
defaults or examples in the unlock UI.
