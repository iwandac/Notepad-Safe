# Headless KDBX core extracted from the pinned KeePassXC 2.7.12 source tree.
# All source inputs are committed under third_party/src. Only the minimized
# Botan build/install products are generated under .deps by the offline
# bootstrap. This file is the small, reviewable KeePassXC source closure.

enable_language(C)

set(NOTEPADSAFE_KEEPASSXC_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/src/keepassxc-2.7.12"
    CACHE PATH "KeePassXC 2.7.12 source root")
set(NOTEPADSAFE_ARGON2_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/src/argon2-20190702"
    CACHE PATH "Argon2 20190702 source root")
set(NOTEPADSAFE_ZLIB_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/src/zlib-1.3.2"
    CACHE PATH "zlib 1.3.2 source root")
set(NOTEPADSAFE_BOTAN_ROOT "${CMAKE_SOURCE_DIR}/.deps/install/botan-3.12.0"
    CACHE PATH "Botan 3.12.0 static installation prefix")

foreach(required_path
        NOTEPADSAFE_KEEPASSXC_SOURCE_DIR
        NOTEPADSAFE_ARGON2_SOURCE_DIR
        NOTEPADSAFE_ZLIB_SOURCE_DIR
        NOTEPADSAFE_BOTAN_ROOT)
    if(NOT EXISTS "${${required_path}}")
        message(FATAL_ERROR
            "${required_path} does not exist: ${${required_path}}. "
            "Run prepare_dependencies.bat first or pass an audited path.")
    endif()
endforeach()

find_package(Botan 3.12.0 EXACT CONFIG REQUIRED
    PATHS "${NOTEPADSAFE_BOTAN_ROOT}/lib/cmake/Botan-3.12.0"
    NO_DEFAULT_PATH
)

set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${NOTEPADSAFE_ZLIB_SOURCE_DIR}"
    "${CMAKE_BINARY_DIR}/_deps/zlib-build" EXCLUDE_FROM_ALL)

add_library(notepadsafe_argon2 STATIC
    "${NOTEPADSAFE_ARGON2_SOURCE_DIR}/src/argon2.c"
    "${NOTEPADSAFE_ARGON2_SOURCE_DIR}/src/core.c"
    "${NOTEPADSAFE_ARGON2_SOURCE_DIR}/src/encoding.c"
    "${NOTEPADSAFE_ARGON2_SOURCE_DIR}/src/thread.c"
    "${NOTEPADSAFE_ARGON2_SOURCE_DIR}/src/blake2/blake2b.c"
    "${NOTEPADSAFE_ARGON2_SOURCE_DIR}/src/opt.c"
)
target_include_directories(notepadsafe_argon2 PUBLIC
    "${NOTEPADSAFE_ARGON2_SOURCE_DIR}/include"
    PRIVATE "${NOTEPADSAFE_ARGON2_SOURCE_DIR}/src"
)
if(MSVC)
    target_compile_definitions(notepadsafe_argon2 PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_library(notepadsafe_zxcvbn STATIC
    "${NOTEPADSAFE_KEEPASSXC_SOURCE_DIR}/src/thirdparty/zxcvbn/zxcvbn.c"
)
target_include_directories(notepadsafe_zxcvbn PUBLIC
    "${NOTEPADSAFE_KEEPASSXC_SOURCE_DIR}/src/thirdparty/zxcvbn"
)

set(_keepassxc_generated_dir "${CMAKE_BINARY_DIR}/generated/keepassxc")
file(MAKE_DIRECTORY "${_keepassxc_generated_dir}")
set(KEEPASSXC_VERSION "2.7.12")
set(WITH_XC_BOTAN3 ON)
set(GIT_HEAD "2.7.12")
configure_file(
    "${NOTEPADSAFE_KEEPASSXC_SOURCE_DIR}/src/config-keepassx.h.cmake"
    "${_keepassxc_generated_dir}/config-keepassx.h"
)
configure_file(
    "${NOTEPADSAFE_KEEPASSXC_SOURCE_DIR}/src/git-info.h.cmake"
    "${_keepassxc_generated_dir}/git-info.h"
)

set(_keepassxc_core_sources
    core/AutoTypeAssociations.cpp
    core/Base32.cpp
    core/Clock.cpp
    core/Config.cpp
    core/CustomData.cpp
    core/Database.cpp
    core/Entry.cpp
    core/EntryAttachments.cpp
    core/EntryAttributes.cpp
    core/FileWatcher.cpp
    core/Group.cpp
    core/Metadata.cpp
    core/ModifiableObject.cpp
    core/PasswordHealth.cpp
    core/PasswordGenerator.cpp
    core/SignalMultiplexer.cpp
    core/TimeDelta.cpp
    core/TimeInfo.cpp
    core/Tools.cpp
    core/Totp.cpp
    core/UrlTools.cpp
    crypto/Crypto.cpp
    crypto/CryptoHash.cpp
    crypto/Random.cpp
    crypto/SymmetricCipher.cpp
    crypto/kdf/Kdf.cpp
    crypto/kdf/AesKdf.cpp
    crypto/kdf/Argon2Kdf.cpp
    format/KeePass2.cpp
    format/KeePass2RandomStream.cpp
    format/KdbxReader.cpp
    format/KdbxWriter.cpp
    format/KdbxXmlReader.cpp
    format/KdbxXmlWriter.cpp
    format/KeePass2Reader.cpp
    format/KeePass2Writer.cpp
    format/Kdbx3Reader.cpp
    format/Kdbx3Writer.cpp
    format/Kdbx4Reader.cpp
    format/Kdbx4Writer.cpp
    keys/ChallengeResponseKey.cpp
    keys/CompositeKey.cpp
    keys/FileKey.cpp
    keys/PasswordKey.cpp
    keys/drivers/YubiKeyStub.cpp
    keys/drivers/YubiKey.h
    streams/HashedBlockStream.cpp
    streams/HashingStream.cpp
    streams/HmacBlockStream.cpp
    streams/LayeredStream.cpp
    streams/qtiocompressor.cpp
    streams/StoreDataStream.cpp
    streams/SymmetricCipherStream.cpp
)
list(TRANSFORM _keepassxc_core_sources
    PREPEND "${NOTEPADSAFE_KEEPASSXC_SOURCE_DIR}/src/")

add_library(notepadsafe_keepassxc_core STATIC ${_keepassxc_core_sources})
set_target_properties(notepadsafe_keepassxc_core PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    AUTOMOC ON
)
target_include_directories(notepadsafe_keepassxc_core PUBLIC
    "${NOTEPADSAFE_KEEPASSXC_SOURCE_DIR}/src"
    "${_keepassxc_generated_dir}"
)
target_compile_definitions(notepadsafe_keepassxc_core PRIVATE
    KEEPASSXC_BUILD_TYPE_RELEASE
)
target_link_libraries(notepadsafe_keepassxc_core PUBLIC
    Qt6::Core
    Qt6::Concurrent
    Qt6::Gui
    Qt6::Network
    Botan::Botan-static
    notepadsafe_argon2
    notepadsafe_zxcvbn
    zlibstatic
)
