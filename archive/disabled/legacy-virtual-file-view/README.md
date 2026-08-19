# Disabled VirtualFileView entry points

The production class previously exposed two synchronous/modal-era helpers:

- `openFileSync(path, error)`, used only by the retired large-file edit tests;
- `buildLineIndexWithProgress(parent, error)`, a compatibility stub that only
  warned and returned success after line-index work moved to
  `AsyncTaskManager`.

Both entry points and the unused progress-parent argument were removed from the
compiled class during the 2026-08-18 static audit. The extracted source is kept
beside this file as `VirtualFileView_sync_open.cpp.disabled`; retired callers
are preserved in `archive/disabled/s2-editing/`.

Do not reintroduce a synchronous full-file index build on the GUI thread.
