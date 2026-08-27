#include "AssemblyShadowBridge.h"

#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
#include "InterpreterImage.h"
#include "StagedAssembly.h"

namespace hybridclr { namespace metadata {

static thread_local ScopedStagingResolver* s_stagingResolver = nullptr;

ScopedStagingResolver::ScopedStagingResolver(const std::vector<StagedAssembly*>& images, StagingResolver resolver, void* context)
    : _images(images), _resolver(resolver), _context(context), _previous(s_stagingResolver)
{
    s_stagingResolver = this;
}

ScopedStagingResolver::~ScopedStagingResolver()
{
    IL2CPP_ASSERT(s_stagingResolver == this);
    s_stagingResolver = _previous;
}

bool AssemblyShadowBridge::IsStaging()
{
    return s_stagingResolver != nullptr;
}

InterpreterImage* AssemblyShadowBridge::GetPrivateImage(uint32_t imageIndex)
{
    if (s_stagingResolver)
        for (StagedAssembly* staged : s_stagingResolver->_images)
            if (staged && staged->interpreterImage && staged->interpreterImage->GetIndex() == imageIndex)
                return staged->interpreterImage;
    return nullptr;
}

bool AssemblyShadowBridge::TryResolveForCurrentThread(const char* name, const Il2CppAssembly*& result)
{
    if (!s_stagingResolver)
        return false;
    result = s_stagingResolver->_resolver ? s_stagingResolver->_resolver(name, s_stagingResolver->_context) : nullptr;
    return true;
}

}}
#endif
