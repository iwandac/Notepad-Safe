#pragma once

#include "SensitiveText.h"

#include <Qt>
#include <memory>

class QSettings;

inline constexpr int EntryIdRole = Qt::UserRole + 1;
inline constexpr int EntryRecycledRole = Qt::UserRole + 2;
inline constexpr int ProtectedClipboardLifetimeMs = 30 * 1000;
inline constexpr int PasswordRevealLifetimeMs = 30 * 1000;

std::unique_ptr<QSettings> createSecuritySettings();
