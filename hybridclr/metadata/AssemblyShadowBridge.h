#pragma once

#include "il2cpp-config.h"

#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
#include <stdint.h>
#include <stdexcept>
#include <vector>

struct Il2CppAssembly;

namespace hybridclr { namespace metadata {

class InterpreterImage;
struct StagedAssembly;
using StagingResolver = const Il2CppAssembly* (*)(const char*, void*);
using StagingFacadeResolver = bool (*)(const char*, std::vector<const Il2CppAssembly*>&, void*);

// A metadata parser must not construct a managed exception while the private
// resolver forbids managed execution. Carry its diagnostic to the staging
// boundary without changing the treatment of unrelated native failures.
class StagedMetadataFailure : public std::runtime_error
{
public:
    explicit StagedMetadataFailure(const char* detail)
        : std::runtime_error(detail ? detail : "Invalid metadata during Assembly Shadow staging.") {}
};

class ScopedStagingResolver
{
public:
    ScopedStagingResolver(const std::vector<StagedAssembly*>& images, StagingResolver resolver, void* context,
        StagingFacadeResolver facadeResolver = nullptr);
    ~ScopedStagingResolver();
    ScopedStagingResolver(const ScopedStagingResolver&) = delete;
    ScopedStagingResolver& operator=(const ScopedStagingResolver&) = delete;

private:
    friend class AssemblyShadowBridge;
    const std::vector<StagedAssembly*>& _images;
    StagingResolver _resolver;
    StagingFacadeResolver _facadeResolver;
    void* _context;
    ScopedStagingResolver* _previous;
};

class AssemblyShadowBridge
{
public:
    static bool IsStaging();
    static InterpreterImage* GetPrivateImage(uint32_t imageIndex);
    // True means TLS owns this lookup, even when result is null. Callers must
    // never fall back to a baseline or ordinary load after a true return.
    static bool TryResolveForCurrentThread(const char* name, const Il2CppAssembly*& result);
    // A logical facade has no Il2CppAssembly. False means the caller must use
    // the strict physical resolver, never ordinary/global assembly fallback.
    static bool TryResolveFacadeForCurrentThread(const char* name, std::vector<const Il2CppAssembly*>& providers);
};

}}
#endif
