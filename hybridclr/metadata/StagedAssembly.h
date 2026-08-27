#pragma once

#include "../CommonDef.h"

#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
#include <atomic>
#include <string>
#include <vector>

namespace hybridclr { namespace metadata {

class InterpreterImage;

// Process-lifetime ownership: no complete InterpreterImage teardown exists.
// Failed/aborted staging is retained privately and the VM seals the transaction.
struct StagedAssembly
{
    InterpreterImage* interpreterImage = nullptr;
    Il2CppAssembly* assembly = nullptr;
    Il2CppImage* image = nullptr;
    std::string canonicalName;
    std::string mvid;
    std::vector<std::string> references;
    const byte* dllBytes = nullptr;
    const byte* pdbBytes = nullptr;
    size_t dllSize = 0;
    size_t pdbSize = 0;
    bool skeletonBuilt = false;
    bool runtimeMetadataInitialized = false;
    bool published = false;
    std::atomic<bool> moduleInitializerRan{false};
    std::atomic<bool> moduleInitializerAttempted{false};
};

}}
#endif
