#pragma once

#include "il2cpp-config.h"

#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
#include <stdint.h>
#include <vector>

struct Il2CppAssembly;

namespace hybridclr { namespace metadata {

class InterpreterImage;
struct StagedAssembly;
using StagingResolver = const Il2CppAssembly* (*)(const char*, void*);

class ScopedStagingResolver
{
public:
    ScopedStagingResolver(const std::vector<StagedAssembly*>& images, StagingResolver resolver, void* context);
    ~ScopedStagingResolver();
    ScopedStagingResolver(const ScopedStagingResolver&) = delete;
    ScopedStagingResolver& operator=(const ScopedStagingResolver&) = delete;

private:
    friend class AssemblyShadowBridge;
    const std::vector<StagedAssembly*>& _images;
    StagingResolver _resolver;
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
};

}}
#endif
