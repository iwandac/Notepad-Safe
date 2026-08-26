#pragma once

#include <memory>

class ISecuritySessionService;
class ISecureDocumentService;

struct SecurityServiceBundle {
    std::unique_ptr<ISecuritySessionService> session;
    std::unique_ptr<ISecureDocumentService> documents;
};

// The composition boundary is the only place that selects the production
// KDBX adapters or the deterministic unavailable adapters.
SecurityServiceBundle createSecurityServiceBundle();
