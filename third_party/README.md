# Vendored dependency sources

`src/` contains the complete pinned source inputs for every runtime dependency
except Qt, which is an exact external static toolchain prerequisite. Production
builds are offline and never populate this directory.

Rules:

- `dependencies.lock.json` records both upstream identity/integrity and the
  SHA-256 of the normalized, real on-disk source tree.
- `src/` contains no nested VCS metadata and no compiled/generated binaries.
- Local compatibility changes are recorded under `patches/`; product features
  must not be implemented by editing an upstream tree directly.
- All build/install output belongs under the ignored `.deps/` directory.
- Run `prepare_dependencies.bat` (after an x64 MSVC environment is active) or
  `build.bat`; neither command performs network access.

See `docs/SUPPLY_CHAIN.md` for the audited update procedure.
