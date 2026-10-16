#include "VaultBackendFactory.h"

#include "IVaultBackend.h"

#ifdef NOTEPADSAFE_HAS_KDBX_BACKEND
#include "KeePassXCVaultBackend.h"
#endif

std::unique_ptr<IVaultBackend> createProductionVaultBackend()
{
    return createProductionVaultBackend(true);
}

std::unique_ptr<IVaultBackend> createProductionVaultBackend(bool keepPreviousVersion)
{
#ifdef NOTEPADSAFE_HAS_KDBX_BACKEND
    return std::make_unique<KeePassXCVaultBackend>(keepPreviousVersion);
#else
    (void)keepPreviousVersion;
    return {};
#endif
}
