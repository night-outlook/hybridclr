#include "StagedAssembly.h"

#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
#include <cstdio>
#include <exception>
#include <limits>
#include <new>

#include "Assembly.h"
#include "AssemblyShadowBridge.h"
#include "InterpreterImage.h"
#include "vm/Image.h"
#include "vm/MetadataLock.h"
#include "vm/Runtime.h"
#include "utils/StringUtils.h"

namespace hybridclr { namespace metadata {
namespace {

using il2cpp::vm::AssemblyShadowError;

bool Contains(size_t length, size_t offset, size_t count)
{
    return offset <= length && count <= length - offset;
}

uint16_t U16(const byte* p) { return GetU2LittleEndian(p); }
uint32_t U32(const byte* p) { return GetU4LittleEndian(p); }
uint64_t U64(const byte* p) { return U32(p) | (uint64_t(U32(p + 4)) << 32); }

// RawImage assumes a trusted PE envelope. Check it before that parser performs
// pointer arithmetic; the same native table schemas remain the source of truth.
bool ValidateDllEnvelope(const byte* bytes, size_t size, bool& supported)
{
    supported = true;
    if (!Contains(size, 0, 64) || bytes[0] != 'M' || bytes[1] != 'Z')
        return false;
    size_t pe = U32(bytes + 0x3c);
    if (!Contains(size, pe, 24) || std::memcmp(bytes + pe, "PE\0\0", 4))
        return false;
    uint16_t sections = U16(bytes + pe + 6);
    uint16_t optionalSize = U16(bytes + pe + 20);
    supported = (U16(bytes + pe + 22) & 0x2000) != 0;
    if (optionalSize != 224 && optionalSize != 240)
        return false;
    size_t optional = pe + 24;
    size_t sectionStart = optional + optionalSize;
    if (!Contains(size, optional, optionalSize) || !Contains(size, sectionStart, size_t(sections) * 40))
        return false;
    if (U16(bytes + optional) != (optionalSize == 224 ? 0x10b : 0x20b))
        return false;
    size_t dirCountOffset = optionalSize == 224 ? 92 : 108;
    if (U32(bytes + optional + dirCountOffset) < 15)
        return false;
    for (uint16_t i = 0; i < sections; ++i)
    {
        const byte* section = bytes + sectionStart + size_t(i) * 40;
        if (uint64_t(U32(section + 12)) + U32(section + 8) > UINT32_MAX ||
            !Contains(size, U32(section + 20), U32(section + 16)))
            return false;
    }
    auto mapRva = [&](uint32_t rva, uint32_t length, size_t& offset) {
        for (uint16_t i = 0; i < sections; ++i)
        {
            const byte* section = bytes + sectionStart + size_t(i) * 40;
            uint32_t start = U32(section + 12);
            if (rva >= start && uint64_t(rva) < uint64_t(start) + U32(section + 8))
            {
                size_t delta = rva - start;
                offset = size_t(U32(section + 20)) + delta;
                return Contains(U32(section + 16), delta, length) && Contains(size, offset, length);
            }
        }
        return false;
    };
    size_t cliDirectory = optional + (optionalSize == 224 ? 208 : 224);
    size_t cli;
    if (U32(bytes + cliDirectory + 4) < 72 || !mapRva(U32(bytes + cliDirectory), 72, cli) || U32(bytes + cli) < 72)
        return false;
    uint32_t flags = U32(bytes + cli + 16);
    supported = supported && (flags & 1) && !(flags & 0x10) && U64(bytes + cli + 64) == 0;
    size_t metadata;
    return U32(bytes + cli + 12) >= 20 && mapRva(U32(bytes + cli + 8), U32(bytes + cli + 12), metadata);
}

bool ValidateBlobHeap(const CliStream& stream)
{
    size_t pos = 0;
    while (pos < stream.size)
    {
        byte first = stream.data[pos];
        size_t prefix;
        uint32_t length;
        if (!(first & 0x80)) { prefix = 1; length = first; }
        else if ((first & 0xc0) == 0x80)
        {
            prefix = 2;
            if (!Contains(stream.size, pos, prefix)) return false;
            length = ((first & 0x3f) << 8) | stream.data[pos + 1];
        }
        else if ((first & 0xe0) == 0xc0)
        {
            prefix = 4;
            if (!Contains(stream.size, pos, prefix)) return false;
            length = (uint32_t(first & 0x1f) << 24) | (uint32_t(stream.data[pos + 1]) << 16) |
                (uint32_t(stream.data[pos + 2]) << 8) | stream.data[pos + 3];
        }
        else return false;
        if (!Contains(stream.size, pos + prefix, length)) return false;
        pos += prefix + length;
    }
    return true;
}

// The native RawImage destructor owns its input. This read-only preparse borrows
// caller bytes and explicitly releases that ownership before the base destructor.
// It never allocates an interpreter index or enters any public image registry.
template<class NativeImage>
class BorrowedCheckedImage : public NativeImage
{
public:
    explicit BorrowedCheckedImage(bool portablePdb = false) : _portablePdb(portablePdb) {}
    ~BorrowedCheckedImage() override { this->_imageData = nullptr; }

    bool ReadString(uint32_t index, std::string& result) const
    {
        if (index >= this->_streamStringHeap.size)
            return false;
        const char* value = reinterpret_cast<const char*>(this->_streamStringHeap.data + index);
        const void* end = std::memchr(value, 0, this->_streamStringHeap.size - index);
        if (!end) return false;
        result.assign(value, static_cast<const char*>(end) - value);
        return true;
    }

    bool ValidBlobIndex(uint32_t index) const
    {
        return index < this->_streamBlobHeap.size;
    }

    bool ReadMvid(uint32_t index, std::string& result) const
    {
        if (!index || index > this->_streamGuidHeap.size / 16) return false;
        const byte* guid = this->_streamGuidHeap.data + size_t(index - 1) * 16;
        char text[37];
        std::snprintf(text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            guid[3], guid[2], guid[1], guid[0], guid[5], guid[4], guid[7], guid[6],
            guid[8], guid[9], guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);
        result = text;
        return true;
    }

    LoadImageErrorCode LoadStreamHeaders(uint32_t rva, uint32_t size) override
    {
        uint32_t offset;
        if (!this->TranslateRVAToImageOffset(rva, offset) || !Contains(this->_imageLength, offset, size) || size < 20)
            return LoadImageErrorCode::BAD_IMAGE;
        const byte* root = this->_imageData + offset;
        if (U32(root) != 0x424a5342) return LoadImageErrorCode::BAD_IMAGE;
        size_t versionSize = U32(root + 12);
        if ((versionSize & 3) || !Contains(size, 16, versionSize) || !Contains(size, 16 + versionSize, 4))
            return LoadImageErrorCode::BAD_IMAGE;
        size_t cursor = 20 + versionSize;
        uint16_t streams = U16(root + 18 + versionSize);
        std::vector<std::string> names;
        bool hasPdbStream = false;
        for (uint16_t i = 0; i < streams; ++i)
        {
            if (!Contains(size, cursor, 9)) return LoadImageErrorCode::BAD_IMAGE;
            const char* name = reinterpret_cast<const char*>(root + cursor + 8);
            size_t available = std::min<size_t>(size - cursor - 8, 16);
            const char* end = static_cast<const char*>(std::memchr(name, 0, available));
            if (!end) return LoadImageErrorCode::BAD_IMAGE;
            std::string key(name, end - name);
            for (const std::string& prior : names)
                if (prior == key) return LoadImageErrorCode::BAD_IMAGE;
            names.push_back(key);
            uint32_t streamOffset = U32(root + cursor);
            uint32_t streamSize = U32(root + cursor + 4);
            if (!Contains(size, streamOffset, streamSize)) return LoadImageErrorCode::BAD_IMAGE;
            if (key == "#Pdb")
            {
                hasPdbStream = true;
                if (streamSize < 32) return LoadImageErrorCode::BAD_IMAGE;
                uint64_t referencedTables = U64(root + streamOffset + 24);
                if ((referencedTables >> 45) || streamSize < 32 + 4 * GetNotZeroBitCount(referencedTables))
                    return LoadImageErrorCode::BAD_IMAGE;
            }
            size_t headerSize = 8 + ((size_t(end - name) / 4) + 1) * 4;
            if (!Contains(size, cursor, headerSize)) return LoadImageErrorCode::BAD_IMAGE;
            cursor += headerSize;
        }
        if (_portablePdb && !hasPdbStream) return LoadImageErrorCode::BAD_IMAGE;
        LoadImageErrorCode error = NativeImage::LoadStreamHeaders(rva, size);
        if (error != LoadImageErrorCode::OK) return error;
        if (!this->_streamTables.data || this->_streamTables.size < 24)
            return LoadImageErrorCode::BAD_IMAGE;
        return ValidateStreams();
    }

    LoadImageErrorCode ValidateStreams() const override
    {
        if (this->_streamStringHeap.size && this->_streamStringHeap.data[this->_streamStringHeap.size - 1] != 0)
            return LoadImageErrorCode::BAD_IMAGE;
        return ValidateBlobHeap(this->_streamUS) && ValidateBlobHeap(this->_streamBlobHeap)
            ? LoadImageErrorCode::OK : LoadImageErrorCode::BAD_IMAGE;
    }

    LoadImageErrorCode LoadTables() override
    {
        const byte* header = this->_streamTables.data;
        if (U32(header) || header[4] != 2 || header[5] != 0 || (header[6] & ~7))
            return LoadImageErrorCode::BAD_IMAGE;
        uint64_t valid = U64(header + 8);
        uint64_t sorted = U64(header + 16);
        if (valid >> TABLE_NUM) return LoadImageErrorCode::BAD_IMAGE;
        this->_4byteStringIndex = header[6] & 1;
        this->_4byteGUIDIndex = header[6] & 2;
        this->_4byteBlobIndex = header[6] & 4;
        size_t cursor = 24;
        for (int i = 0; i <= MAX_TABLE_INDEX; ++i)
        {
            this->_tables[i] = {};
            if (valid & (uint64_t(1) << i))
            {
                if (!Contains(this->_streamTables.size, cursor, 4)) return LoadImageErrorCode::BAD_IMAGE;
                this->_tables[i].rowNum = U32(header + cursor);
                cursor += 4;
            }
        }
        this->BuildTableRowMetas();
        for (int i = 0; i <= MAX_TABLE_INDEX; ++i)
        {
            if (!(valid & (uint64_t(1) << i))) continue;
            uint32_t width = 0;
            for (auto& column : this->_tableRowMetas[i])
            {
                column.offset = width;
                width += column.size;
            }
            uint32_t rows = this->_tables[i].rowNum;
            uint64_t bytes = uint64_t(width) * rows;
            if (!width || bytes > this->_streamTables.size || !Contains(this->_streamTables.size, cursor, size_t(bytes)))
                return LoadImageErrorCode::BAD_IMAGE;
            this->_tables[i] = { header + cursor, width, rows, true, (sorted & (uint64_t(1) << i)) != 0 };
            cursor += size_t(bytes);
        }
        return LoadImageErrorCode::OK;
    }

private:
    bool _portablePdb;
};

bool IsSimpleName(const std::string& name)
{
    return !name.empty() && name.find_first_of("/\\,\r\n\t") == std::string::npos;
}

AssemblyShadowError ParseIdentity(const byte* dll, size_t size, std::string& name,
    std::string* mvid, std::vector<std::string>* references, std::string& detail)
{
    if (!dll || !size || size > UINT32_MAX / 4)
    {
        detail = "DLL bytes are null, empty, or exceed the interpreter index size limit";
        return AssemblyShadowError::InvalidArgument;
    }
    bool supported;
    if (!ValidateDllEnvelope(dll, size, supported))
    {
        detail = "Invalid DLL PE/CLI envelope";
        return AssemblyShadowError::BadImage;
    }
    if (!supported)
    {
        detail = "Only IL-only managed DLLs without a native entry point are supported";
        return AssemblyShadowError::UnsupportedAssembly;
    }
    BorrowedCheckedImage<RawImage> raw;
    if (raw.Load(dll, size) != LoadImageErrorCode::OK || raw.GetTableRowNum(TableType::ASSEMBLY) != 1 ||
        raw.GetTableRowNum(TableType::MODULE) != 1 || !raw.GetTableRowNum(TableType::TYPEDEF))
    {
        detail = "Invalid DLL metadata or missing assembly/module definition";
        return AssemblyShadowError::BadImage;
    }
    if (raw.GetTableRowNum(TableType::EXPORTEDTYPE) || raw.GetTableRowNum(TableType::FILE))
    {
        detail = "Staged interpreter images do not support exported types or multi-module assemblies";
        return AssemblyShadowError::UnsupportedAssembly;
    }
    TbAssembly assembly = raw.ReadAssembly(1);
    TbModule module = raw.ReadModule(1);
    std::string ignored, parsedMvid;
    if (!raw.ReadString(assembly.name, name) || !IsSimpleName(name) || !raw.ReadString(assembly.locale, ignored) ||
        !raw.ValidBlobIndex(assembly.publicKey) || !raw.ReadString(module.name, ignored) || !raw.ReadMvid(module.mvid, parsedMvid))
    {
        detail = "Invalid assembly name, module identity, or assembly heap index";
        return AssemblyShadowError::BadImage;
    }
    if (mvid) *mvid = parsedMvid;
    if (references) references->clear();
    for (uint32_t row = 1, count = raw.GetTableRowNum(TableType::ASSEMBLYREF); row <= count; ++row)
    {
        TbAssemblyRef reference = raw.ReadAssemblyRef(row);
        std::string referenceName;
        if (!raw.ReadString(reference.name, referenceName) || !IsSimpleName(referenceName) ||
            !raw.ReadString(reference.locale, ignored) || !raw.ValidBlobIndex(reference.publicKeyOrToken) ||
            !raw.ValidBlobIndex(reference.hashValue))
        {
            detail = "Invalid AssemblyRef identity or heap index";
            return AssemblyShadowError::BadImage;
        }
        if (references) references->push_back(referenceName);
    }
    return AssemblyShadowError::Success;
}

std::string ManagedExceptionDetail(const Il2CppExceptionWrapper& error)
{
    if (error.ex && error.ex->message)
        return il2cpp::utils::StringUtils::Utf16ToUtf8(error.ex->message->chars, error.ex->message->length);
    return "managed exception without a message";
}

byte* CopyOwnedBytes(const byte* bytes, size_t size)
{
    byte* owned = static_cast<byte*>(HYBRIDCLR_MALLOC(size));
    if (!owned) throw std::bad_alloc();
    std::memcpy(owned, bytes, size);
    return owned;
}

} // namespace

AssemblyShadowError Assembly::ReadStagedAssemblyIdentity(const byte* dll, size_t dllLength, std::string& name, std::string& detail)
{
    name.clear();
    detail.clear();
    try { return ParseIdentity(dll, dllLength, name, nullptr, nullptr, detail); }
    catch (const std::exception& error) { detail = error.what(); }
    catch (...) { detail = "Unexpected native failure while reading DLL identity"; }
    return AssemblyShadowError::InternalError;
}

AssemblyShadowError Assembly::CreateStagedSkeleton(const byte* dll, size_t dllLength, const byte* pdb, size_t pdbLength,
    StagedAssembly*& staged, std::string& detail)
{
    staged = nullptr;
    detail.clear();
    try
    {
        // All identity/format rejection remains before the monotonic index
        // allocator. Main additionally checks exact closure membership first.
        std::string name, mvid;
        std::vector<std::string> references;
        AssemblyShadowError error = ParseIdentity(dll, dllLength, name, &mvid, &references, detail);
        if (error != AssemblyShadowError::Success) return error;
        if ((!pdb && pdbLength) || (pdb && !pdbLength) || pdbLength > UINT32_MAX)
        {
            detail = "PDB pointer/length pair is invalid";
            return AssemblyShadowError::InvalidArgument;
        }
        if (pdb)
        {
            BorrowedCheckedImage<PDBImage> symbols(true);
            if (symbols.Load(pdb, pdbLength) != LoadImageErrorCode::OK)
            {
                detail = "Invalid portable PDB metadata";
                return AssemblyShadowError::BadImage;
            }
        }
        il2cpp::os::FastAutoLock lock(&il2cpp::vm::g_MetadataLock);
        // Publish the private owner to the transaction before any retained
        // allocation, including partial construction failures.
        staged = new StagedAssembly();
        staged->canonicalName.swap(name);
        staged->mvid.swap(mvid);
        staged->references.swap(references);
        uint32_t index = InterpreterImage::AllocImageIndex(static_cast<uint32_t>(dllLength));
        if (index == kInvalidImageIndex)
        {
            detail = "Interpreter image index capacity exhausted";
            return AssemblyShadowError::InternalError;
        }
        staged->interpreterImage = new InterpreterImage(index);
        byte* ownedDll = CopyOwnedBytes(dll, dllLength);
        if (staged->interpreterImage->Load(ownedDll, dllLength) != LoadImageErrorCode::OK)
        {
            detail = "Native DLL load failed after validated identity preparse";
            return AssemblyShadowError::BadImage;
        }
        staged->dllBytes = ownedDll;
        staged->dllSize = dllLength;
        if (pdb)
        {
            byte* ownedPdb = CopyOwnedBytes(pdb, pdbLength);
            if (staged->interpreterImage->LoadPDB(ownedPdb, pdbLength) != LoadImageErrorCode::OK)
            {
                detail = "Native PDB load failed after validated preparse";
                return AssemblyShadowError::BadImage;
            }
            staged->pdbBytes = ownedPdb;
            staged->pdbSize = pdbLength;
        }
        void* assemblyMemory = HYBRIDCLR_MALLOC_ZERO(sizeof(Il2CppAssembly));
        if (!assemblyMemory) throw std::bad_alloc();
        staged->assembly = new (assemblyMemory) Il2CppAssembly;
        void* imageMemory = HYBRIDCLR_MALLOC_ZERO(sizeof(Il2CppImage));
        if (!imageMemory) throw std::bad_alloc();
        staged->image = new (imageMemory) Il2CppImage;
        staged->interpreterImage->InitBasic(staged->image, false);
        staged->interpreterImage->BuildIl2CppAssembly(staged->assembly);
        staged->assembly->image = staged->image;
        staged->interpreterImage->BuildIl2CppImage(staged->image);
        staged->image->name = ConcatNewString(staged->assembly->aname.name, ".dll");
        staged->image->nameNoExt = staged->assembly->aname.name;
        staged->image->assembly = staged->assembly;
        staged->skeletonBuilt = true;
        return AssemblyShadowError::Success;
    }
    catch (const Il2CppExceptionWrapper& error) { detail = ManagedExceptionDetail(error); }
    catch (const std::exception& error) { detail = error.what(); }
    catch (...) { detail = "Unexpected native failure constructing private skeleton"; }
    return AssemblyShadowError::InternalError;
}

AssemblyShadowError Assembly::InitializeStagedRuntimeMetadata(StagedAssembly* staged, std::string& detail)
{
    detail.clear();
    if (!staged || !staged->skeletonBuilt || staged->runtimeMetadataInitialized || staged->published ||
        !AssemblyShadowBridge::IsStaging() || AssemblyShadowBridge::GetPrivateImage(staged->interpreterImage->GetIndex()) != staged->interpreterImage)
    {
        detail = "Runtime metadata requires a private skeleton and the complete staging resolver scope";
        return AssemblyShadowError::InvalidState;
    }
    try
    {
        il2cpp::os::FastAutoLock lock(&il2cpp::vm::g_MetadataLock);
        // Bind even references not eagerly touched by layout/vtable setup. Lazy
        // metadata after commit must not consult a constructor-cached baseline.
        for (size_t i = 0; i < staged->references.size(); ++i)
            staged->interpreterImage->GetReferencedAssembly(static_cast<int32_t>(i), nullptr, 0);
        staged->interpreterImage->InitRuntimeMetadatas();
        staged->runtimeMetadataInitialized = true;
        return AssemblyShadowError::Success;
    }
    catch (const Il2CppExceptionWrapper& error)
    {
        detail = staged->canonicalName + ": " + ManagedExceptionDetail(error);
        return AssemblyShadowError::ReferenceResolutionFailed;
    }
    catch (const std::exception& error) { detail = staged->canonicalName + ": " + error.what(); }
    catch (...) { detail = staged->canonicalName + ": unexpected runtime metadata failure"; }
    return AssemblyShadowError::InternalError;
}

void Assembly::PublishStagedImage(StagedAssembly* staged)
{
    // Called only from the VM's pre-reserved, locked batch publication. This
    // fixed-index store cannot allocate, register an assembly, or execute code.
    IL2CPP_ASSERT(staged && staged->skeletonBuilt && staged->runtimeMetadataInitialized && !staged->published);
    InterpreterImage::RegisterImage(staged->interpreterImage);
    staged->published = true;
}

AssemblyShadowError Assembly::RunStagedModuleInitializer(StagedAssembly* staged, std::string& detail)
{
    detail.clear();
    if (!staged || !staged->published || AssemblyShadowBridge::IsStaging() || staged->moduleInitializerAttempted.exchange(true))
    {
        detail = "Module initialization requires a published image and is attempted only once";
        return AssemblyShadowError::InvalidState;
    }
    try
    {
        Il2CppClass* module = il2cpp::vm::Image::ClassFromName(staged->image, "", "<Module>");
        if (module) il2cpp::vm::Runtime::ClassInit(module);
        staged->moduleInitializerRan.store(true);
        return AssemblyShadowError::Success;
    }
    catch (const Il2CppExceptionWrapper& error) { detail = staged->canonicalName + ": " + ManagedExceptionDetail(error); }
    catch (const std::exception& error) { detail = staged->canonicalName + ": " + error.what(); }
    catch (...) { detail = staged->canonicalName + ": unexpected module initializer failure"; }
    return AssemblyShadowError::ModuleInitializerFailed;
}

}}
#endif
