#pragma once

#include "il2cpp-config.h"
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
#include "RawImageBase.h"
#include "utils/sha1.h"

namespace hybridclr { namespace metadata {

// Owned by InterpreterImage; string/blob pointers borrow its process-lifetime
// metadata. This is not an Il2CppAssembly and never represents a fake facade.
struct AssemblyReferenceIdentity
{
    Il2CppAssemblyName name;
    bool hasPublicKeyToken;
};

inline AssemblyReferenceIdentity ReadDeclaredAssemblyReference(const RawImageBase& raw, const TbAssemblyRef& reference)
{
    AssemblyReferenceIdentity result = {};
    Il2CppAssemblyName& declared = result.name;
    declared.name = raw.GetStringFromRawIndex(reference.name);
    declared.culture = raw.GetStringFromRawIndex(reference.locale);
    declared.flags = reference.flags;
    declared.major = reference.majorVersion;
    declared.minor = reference.minorVersion;
    declared.build = reference.buildNumber;
    declared.revision = reference.revisionNumber;
    BlobReader key = raw.GetBlobReaderByRawIndex(reference.publicKeyOrToken);
    result.hasPublicKeyToken = key.GetLength() != 0;
    if (reference.flags & 1) // AssemblyFlags.PublicKey: full key, not a token.
    {
        if (key.GetLength() > INT32_MAX) RaiseBadImageException("AssemblyRef public key is too large");
        declared.public_key = raw.GetBlobFromRawIndex(reference.publicKeyOrToken);
        if (key.GetLength())
        {
            uint8_t digest[20];
            sha1_get_digest(key.GetData(), static_cast<int>(key.GetLength()), digest);
            for (size_t byte = 0; byte < sizeof(declared.public_key_token); ++byte)
                declared.public_key_token[byte] = digest[19 - byte];
        }
    }
    else if (key.GetLength())
    {
        if (key.GetLength() != sizeof(declared.public_key_token))
            RaiseBadImageException("AssemblyRef public key token must contain eight bytes");
        memcpy(declared.public_key_token, key.GetData(), sizeof(declared.public_key_token));
    }
    return result;
}

}}
#endif
