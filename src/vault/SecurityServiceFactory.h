#pragma once

#include "SecureDocumentService.h"
#include "SecuritySessionService.h"

#include <memory>

struct SecurityServiceBundle {
    std::unique_ptr<ISecuritySessionService> session;
    std::unique_ptr<ISecureDocumentService> documents;
};

// The composition boundary is the only place that selects the production
// KDBX adapters or the deterministic unavailable adapters.
SecurityServiceBundle createSecurityServiceBundle();
