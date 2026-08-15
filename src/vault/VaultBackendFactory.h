#pragma once

#include <memory>

class IVaultBackend;

// Returns the pinned production implementation when the product was built
// with NOTEPADSAFE_ENABLE_KDBX_BACKEND, otherwise nullptr. Keeping this choice
// behind a factory prevents UI code from depending on KeePassXC types.
std::unique_ptr<IVaultBackend> createProductionVaultBackend();
