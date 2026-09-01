#pragma once

#include <QString>

// One source of truth for every component that owns or coordinates files in
// the Notepad Safe profile. An absolute NOTEPADSAFE_DATADIR is the supported
// test/profile override; otherwise the established LocalAppData/NotepadSafe
// location is used.
QString resolveNotepadSafeDataRoot();
