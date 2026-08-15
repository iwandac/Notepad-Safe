#include "VaultBackendFactory.h"

#include "IVaultBackend.h"

#ifdef NOTEPADSAFE_HAS_KDBX_BACKEND
#include "KeePassXCVaultBackend.h"
#endif

std::unique_ptr<IVaultBackend> createProductionVaultBackend()
{
#ifdef NOTEPADSAFE_HAS_KDBX_BACKEND
    return std::make_unique<KeePassXCVaultBackend>();
#else
    return {};
#endif
}
