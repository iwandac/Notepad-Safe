# Disabled S2 editing experiments

This directory preserves the pre-2026-08-18 mmap + `PatchList` experiments
that attempted to make 32 MiB–2 GiB files editable.

They are historical reference only:

- none of these files is referenced by `CMakeLists.txt`;
- they are not compiled, linked, registered with CTest, or run by repository
  build/test scripts;
- their assumptions conflict with the current product contract: S2/S3 and
  B2/B3 are windowed mmap, browse/search/copy, and read-only;
- active B1 binary editing still legitimately uses `VirtualFileView` and
  `PatchList` from `src/` and is not part of this archive.

Do not restore an archived target without a new architecture decision and a
real GUI acceptance slice covering continuous browse, editing, save, recovery,
memory pressure, and source-file integrity.
