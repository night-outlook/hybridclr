#include "InterpreterImage.h"
#include "InterpreterTypeCacheInsertion.h"

#include <cstring>
#include <cmath>
#include <iostream>
#include <algorithm>
#include <limits>

#include "il2cpp-class-internals.h"
#include "vm/GlobalMetadata.h"
#include "vm/Type.h"
#include "vm/Field.h"
#include "vm/Object.h"
#include "vm/Runtime.h"
#include "vm/Array.h"
#include "vm/MetadataLock.h"
#include "vm/MetadataCache.h"
#include "vm/MetadataAlloc.h"
#include "vm/String.h"
#include "vm/Reflection.h"
#include "metadata/FieldLayout.h"
#include "metadata/Il2CppTypeCompare.h"
#include "metadata/GenericMetadata.h"
#if HYBRIDCLR_UNITY_2021_OR_NEW
#include "metadata/CustomAttributeCreator.h"
#endif
#include "os/Atomic.h"
#include "icalls/mscorlib/System/MonoCustomAttrs.h"

#include "MetadataModule.h"
#include "MetadataUtil.h"
#include "InterpreterMetadataRange.h"
#include "InterpreterMetadataCounts.h"
#include "InterpreterImageAdmission.h"
#include "InterpreterMetadataIndexRuntime.h"
#include "ClassFieldLayoutCalculator.h"
#include "MetadataPool.h"

#include "../interpreter/Engine.h"
#include "../interpreter/InterpreterModule.h"

namespace hybridclr
{
namespace metadata
{

    struct InterpreterImageMetadataDecoder
    {
        InterpreterMetadataRange::DecodedIndex operator()(uint32_t encoded) const
        {
            if (encoded == static_cast<uint32_t>(kInvalidIndex))
                return InterpreterMetadataRange::DecodedIndex();
            return InterpreterMetadataRange::DecodedIndex(true,
                DecodeImageIndex(static_cast<int32_t>(encoded)),
                DecodeMetadataIndex(static_cast<int32_t>(encoded)));
        }
    };

    static void RaiseInvalidMetadataRange(const char* message,
        InterpreterMetadataRange::Error error)
    {
        if (error != InterpreterMetadataRange::Error::None)
            RaiseBadImageException(message);
    }

    static uint64_t s_ordinaryImageAllocations = 0;
    static uint64_t s_shadowImageAllocations = 0;
    static uint64_t s_reservedImageCount = 0;

    void InterpreterImage::Initialize()
    {
        if (InterpreterMetadataIndexRuntime::Initialize() != InterpreterMetadataIndexRuntime::Error::None)
            RaiseExecutionEngineException("failed to initialize sparse interpreter metadata index runtime");
    }

    uint32_t InterpreterImage::AllocImageIndex(uint64_t dllLength, bool shadow)
    {
        InterpreterMetadataIndexRuntime::Codec::Stats stats{};
        if (InterpreterMetadataIndexRuntime::GetStats(stats) != InterpreterMetadataIndexRuntime::Error::None)
            return kInvalidImageIndex;
        const InterpreterImageAdmission::Report admission =
            InterpreterImageAdmission::Evaluate(stats.reservationCount, &dllLength, 1);
        if (!admission.IsSuccess())
            return kInvalidImageIndex;
        std::vector<InterpreterMetadataIndexRuntime::Reservation> reservations;
        if (InterpreterMetadataIndexRuntime::ReserveImages(1, reservations) != InterpreterMetadataIndexRuntime::Error::None)
            return kInvalidImageIndex;
        if (shadow) ++s_shadowImageAllocations;
        else ++s_ordinaryImageAllocations;
        return reservations[0].imageId;
    }

    InterpreterImageAdmission::Report InterpreterImage::ReserveImageBudget(
        const std::vector<uint64_t>& sizes, std::vector<uint32_t>& imageIndices,
        InterpreterMetadataIndexRuntime::Error& runtimeError)
    {
        il2cpp::os::FastAutoLock lock(&il2cpp::vm::g_MetadataLock);
        imageIndices.clear();
        runtimeError = InterpreterMetadataIndexRuntime::Error::None;
        InterpreterMetadataIndexRuntime::Codec::Stats stats{};
        runtimeError = InterpreterMetadataIndexRuntime::GetStats(stats);
        if (runtimeError != InterpreterMetadataIndexRuntime::Error::None)
            return InterpreterImageAdmission::Evaluate(InterpreterImageAdmission::kMaximumImages + 1, nullptr, 0);
        InterpreterImageAdmission::Report report = InterpreterImageAdmission::Evaluate(
            stats.reservationCount, sizes.empty() ? nullptr : sizes.data(), sizes.size());
        if (!report.IsSuccess())
            return report;
        if (sizes.empty())
        {
            report.error = InterpreterImageAdmission::Error::InvalidInput;
            report.firstFailureIndex = 0;
            return report;
        }
        // Allocate every fallible output buffer before ReserveImages advances the
        // process-lifetime ledger. Only fixed-width assignments and a noexcept
        // swap may follow a successful reservation.
        std::vector<uint32_t> preparedIndices(sizes.size());
        std::vector<InterpreterMetadataIndexRuntime::Reservation> reservations;
        runtimeError = InterpreterMetadataIndexRuntime::ReserveImages(static_cast<uint32_t>(sizes.size()), reservations);
        if (runtimeError != InterpreterMetadataIndexRuntime::Error::None)
            return report;
        for (size_t i = 0; i < reservations.size(); ++i)
            preparedIndices[i] = reservations[i].imageId;
        imageIndices.swap(preparedIndices);
        s_reservedImageCount += sizes.size();
        return report;
    }

    InterpreterMetadataIndexRuntime::Error InterpreterImage::GetMetadataIndexStats(
        InterpreterMetadataIndexRuntime::Codec::Stats& stats)
    {
        return InterpreterMetadataIndexRuntime::GetStats(stats);
    }

    InterpreterMetadataIndexRuntime::Error InterpreterImage::GetMetadataCapacitySnapshot(
        InterpreterMetadataIndexRuntime::Codec::Stats& stats,
        uint64_t& ordinary, uint64_t& shadow, uint64_t& reserved)
    {
        // Ordinary and Shadow reservations take g_MetadataLock before the
        // codec reservation mutex. Capture the diagnostic tuple in that same
        // order so every counter describes one observable runtime instant.
        il2cpp::os::FastAutoLock lock(&il2cpp::vm::g_MetadataLock);
        InterpreterMetadataIndexRuntime::Error error = InterpreterMetadataIndexRuntime::GetStats(stats);
        if (error != InterpreterMetadataIndexRuntime::Error::None)
            return error;
        ordinary = s_ordinaryImageAllocations;
        shadow = s_shadowImageAllocations;
        reserved = s_reservedImageCount;
        return InterpreterMetadataIndexRuntime::Error::None;
    }

    void InterpreterImage::GetImageAllocationCounts(uint64_t& ordinary, uint64_t& shadow, uint64_t& reserved)
    {
        il2cpp::os::FastAutoLock lock(&il2cpp::vm::g_MetadataLock);
        ordinary = s_ordinaryImageAllocations;
        shadow = s_shadowImageAllocations;
        reserved = s_reservedImageCount;
    }

    void InterpreterImage::RecordReservedShadowAllocation()
    {
        // Caller holds the metadata lock. Reservation remains permanently charged.
        ++s_shadowImageAllocations;
    }

	InterpreterMetadataIndexRuntime::Error InterpreterImage::FinalizeImage(InterpreterImage* image)
	{
		if (!image || image->GetIndex() == 0)
            return InterpreterMetadataIndexRuntime::Error::InvalidImage;
        return InterpreterMetadataIndexRuntime::Finalize(image->GetIndex(), image->ComputeFinalIndexLowEnd());
	}

	InterpreterMetadataIndexRuntime::Error InterpreterImage::RegisterImage(InterpreterImage* image)
	{
		if (!image || image->GetIndex() == 0)
            return InterpreterMetadataIndexRuntime::Error::InvalidImage;
        return InterpreterMetadataIndexRuntime::Publish(image->GetIndex());
	}

    InterpreterMetadataIndexRuntime::Error InterpreterImage::RegisterImagesBatch(
        const uint32_t* imageIndices, size_t count)
    {
        return InterpreterMetadataIndexRuntime::PublishBatch(imageIndices, count);
	}

    InterpreterMetadataIndexRuntime::Error InterpreterImage::AbortImage(uint32_t imageIndex)
    {
        return InterpreterMetadataIndexRuntime::Abort(imageIndex);
	}

	void InterpreterImage::InitBasic(Il2CppImage* image, bool publish)
	{
		SetIl2CppImage(image);
		if (publish)
			RaiseExecutionEngineException("interpreter image publication requires final footprint sealing");
		else
			_nameToAssemblies.clear();
	}

	void InterpreterImage::BuildIl2CppAssembly(Il2CppAssembly* ass)
	{
		ass->token = EncodeToken(TableType::ASSEMBLY, 1);
		ass->referencedAssemblyStart = EncodeWithIndex(1);
		ass->referencedAssemblyCount = _rawImage->GetTableRowNum(TableType::ASSEMBLYREF);

		TbAssembly data = _rawImage->ReadAssembly(1);
		auto& aname = ass->aname;
		aname.hash_alg = data.hashAlgId;
		aname.major = data.majorVersion;
		aname.minor = data.minorVersion;
		aname.build = data.buildNumber;
		aname.revision = data.revisionNumber;
		aname.flags = data.flags;
		aname.public_key = _rawImage->GetBlobFromRawIndex(data.publicKey);
		aname.name = _rawImage->GetStringFromRawIndex(data.name);
		aname.culture = _rawImage->GetStringFromRawIndex(data.locale);
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        _referenceRequester = ass;
        // These identities belong to the declaring image, not to whichever
        // physical AOT/provider currently satisfies a reference. Retain the
        // complete rows before publication, including logical facade rows.
        _declaredAssemblyReferences.resize(ass->referencedAssemblyCount);
        for (int32_t index = 0; index < ass->referencedAssemblyCount; ++index)
        {
            _declaredAssemblyReferences[index] = ReadDeclaredAssemblyReference(*_rawImage, _rawImage->ReadAssemblyRef(index + 1));
        }
#endif
	}

	void InterpreterImage::BuildIl2CppImage(Il2CppImage* image2)
	{
		image2->typeCount = _rawImage->GetTableRowNum(TableType::TYPEDEF);
		image2->exportedTypeCount = _rawImage->GetTableRowNum(TableType::EXPORTEDTYPE);
		image2->customAttributeCount = _rawImage->GetTableRowNum(TableType::CUSTOMATTRIBUTE);

#if HYBRIDCLR_UNITY_2019
		image2->typeStart = EncodeWithIndex(0);
		image2->customAttributeStart = EncodeWithIndex(0);
		image2->entryPointIndex = EncodeWithIndexExcept0(_rawImage->GetEntryPointToken());
		image2->exportedTypeStart = EncodeWithIndex(0);
#else
		Il2CppImageGlobalMetadata* metadataImage = (Il2CppImageGlobalMetadata*)HYBRIDCLR_METADATA_MALLOC(sizeof(Il2CppImageGlobalMetadata));
		metadataImage->typeStart = EncodeWithIndex(0);
		metadataImage->customAttributeStart = EncodeWithIndex(0);
		metadataImage->entryPointIndex = EncodeWithIndexExcept0(_rawImage->GetEntryPointToken());
		metadataImage->exportedTypeStart = EncodeWithIndex(0);
		metadataImage->image = image2;
		image2->metadataHandle = reinterpret_cast<Il2CppMetadataImageHandle>(metadataImage);
#endif

		image2->nameToClassHashTable = nullptr;
		image2->codeGenModule = nullptr;

		image2->token = EncodeWithIndex(0); // TODO
		image2->dynamic = 0;
	}

	void InterpreterImage::InitRuntimeMetadatas()
	{
		IL2CPP_ASSERT(_rawImage->GetTable(TableType::EXPORTEDTYPE).rowNum == 0);

		InitGenericParamDefs0();
		InitTypeDefs_0();
		InitMethodDefs0();
		InitGenericParamDefs();
		InitNestedClass(); // must before typedefs1, because parent may be nested class
		InitTypeDefs_1();

		InitGenericParamConstraintDefs();

		InitParamDefs();
		InitMethodDefs();
		InitFieldDefs();
		InitFieldLayouts();
		InitFieldRVAs();
		InitBlittables();
		InitMethodImpls0();
		InitProperties();
		InitEvents();
		InitMethodSemantics();
		InitConsts();
		InitCustomAttributes();
		InitModuleRefs();
		InitImplMaps();
		InitClassLayouts0();
		InitHasFinalizers();
		InitTypeDefs_2();
		InitClassLayouts();
		InitInterfaces();
		InitClass();
		InitVTables();

		Il2CppHashMap<const Il2CppType*, uint32_t, Il2CppTypeHashShallow, Il2CppTypeEqualityComparerShallow> temp;
		_type2Indexs.swap(temp);

		delete _paramRawIndex2ActualParamIndex;
		_paramRawIndex2ActualParamIndex = nullptr;
	}

	uint64_t InterpreterImage::ComputeFinalIndexLowEnd() const
	{
		const uint64_t rawLimit = uint64_t(1) << 31;
		uint64_t lowEnd = 1; // image token/raw zero must be representable
		auto includeLast = [&lowEnd, rawLimit](uint64_t last)
		{
			if (last >= rawLimit)
				RaiseExecutionEngineException("interpreter metadata footprint exceeds raw index domain");
			lowEnd = std::max(lowEnd, last + 1);
		};
		// Table rows are one-based. List starts can include count+1 as the
		// terminal value, so that encoded endpoint must itself fit below L.
		for (int table = 0; table < TABLE_NUM; ++table)
			includeLast(uint64_t(_rawImage->GetTableRowNum(static_cast<TableType>(table))) + 1);
		const uint32_t stringBytes = _rawImage->GetStringHeapSize();
		if (stringBytes != 0)
			includeLast(uint64_t(stringBytes) - 1);

		// Actual synthesized vectors can exceed individual input table counts.
		// Conservatively admit their terminal coordinate as well as elements.
		const uint64_t extents[] = {
			_typesDefines.size(), _exportedTypeDefines.size(), _interfaceDefines.size(),
			_interfaceOffsets.size(), _methodDefines.size(), _params.size(),
			_paramDefaultValues.size(), _genericParams.size(), _genericConstraints.size(),
			_genericContainers.size(), _fieldDetails.size(), _fieldDefaultValues.size(),
			_nestedTypeDefineIndexs.size(), _customAttributeHandles.size(),
			_customAttribues.size(), _propeties.size(), _events.size(), _moduleRefs.size()
		};
		for (uint64_t extent : extents)
			includeLast(extent);

		uint64_t typeEnd = _types.size();
		auto addFutureTypes = [&typeEnd, rawLimit](uint64_t count)
		{
			if (typeEnd > rawLimit || count > rawLimit - typeEnd)
				RaiseExecutionEngineException("interpreter lazy type footprint exceeds raw index domain");
			typeEnd += count;
		};
		// Constraint and interface caches publish at most once per row under
		// g_MetadataLock. Attribute batches append only after a complete range
		// succeeds; charge validated blob bytes once per row occurrence, even
		// when multiple rows share the same blob offset.
		addFutureTypes(_genericConstraints.size());
		addFutureTypes(_interfaceDefines.size());
#if HYBRIDCLR_UNITY_2021_OR_NEW
		for (const CustomAttribute& attribute : _customAttribues)
			if (attribute.value != 0)
				addFutureTypes(_rawImage->GetBlobReaderByRawIndex(attribute.value).GetLength());
#endif
		if (typeEnd != 0)
			includeLast(typeEnd - 1);
		return lowEnd;
	}

	void InterpreterImage::InitTypeDefs_0()
	{
		const Table& typeDefTb = _rawImage->GetTable(TableType::TYPEDEF);
		_typesDefines.resize(typeDefTb.rowNum);
		_typeDetails.resize(typeDefTb.rowNum);
		for (uint32_t i = 0, n = typeDefTb.rowNum; i < n; i++)
		{
			Il2CppTypeDefinition& cur = _typesDefines[i];
			TypeDefinitionDetail& typeDetail = _typeDetails[i];
			typeDetail.typeSizes = {};

			uint32_t rowIndex = i + 1;
			TbTypeDef data = _rawImage->ReadTypeDef(rowIndex);

			cur = {};

			cur.genericContainerIndex = kGenericContainerIndexInvalid;
			cur.declaringTypeIndex = kTypeDefinitionIndexInvalid;
			cur.elementTypeIndex = kTypeDefinitionIndexInvalid;

			cur.token = EncodeToken(TableType::TYPEDEF, rowIndex);

			bool isValueType = data.extends && IsValueTypeFromToken(DecodeTypeDefOrRefOrSpecCodedIndexTableType(data.extends), DecodeTypeDefOrRefOrSpecCodedIndexRowIndex(data.extends));
			Il2CppType* cppType = MetadataMallocT<Il2CppType>();
			cppType->type = isValueType ? IL2CPP_TYPE_VALUETYPE : IL2CPP_TYPE_CLASS;
			SET_IL2CPPTYPE_VALUE_TYPE(*cppType, isValueType);
			cppType->data.typeHandle = (Il2CppMetadataTypeHandle)&cur;
			cur.byvalTypeIndex = AddIl2CppTypeCache(cppType);
#if HYBRIDCLR_UNITY_2019
			Il2CppType* byRefType = MetadataMallocT<Il2CppType>();
			*byRefType = *cppType;
			byRefType->byref = 1;
			cur.byrefTypeIndex = AddIl2CppTypeCache(byRefType);
#endif

			if (IsInterface(cur.flags))
			{
				cur.interfaceOffsetsStart = EncodeWithIndex(0);
				cur.interface_offsets_count = 0;
				cur.vtableStart = EncodeWithIndex(0);
				cur.vtable_count = 0;
			}
			else
			{
				cur.interfaceOffsetsStart = 0;
				cur.interface_offsets_count = 0;
				cur.vtableStart = 0;
				cur.vtable_count = 0;
			}
		}
	}

	void InterpreterImage::InitTypeDefs_1()
	{
		const Table& typeDefTb = _rawImage->GetTable(TableType::TYPEDEF);
		const uint32_t fieldRowCount = _rawImage->GetTableRowNum(TableType::FIELD);
		const uint32_t methodRowCount = _rawImage->GetTableRowNum(TableType::METHOD);
		const InterpreterImageMetadataDecoder decoder;
		for (uint32_t i = 0, n = typeDefTb.rowNum; i < n; i++)
		{
			Il2CppTypeDefinition& last = _typesDefines[i > 0 ? i - 1 : 0];
			Il2CppTypeDefinition& cur = _typesDefines[i];
			uint32_t rowIndex = i + 1;
			TbTypeDef data = _rawImage->ReadTypeDef(rowIndex); // token from 1

			cur.flags = data.flags;
			cur.nameIndex = EncodeWithIndex(data.typeName);
			cur.namespaceIndex = EncodeWithIndex(data.typeNamespace);

			cur.fieldStart = EncodeWithIndex(data.fieldList - 1);
			cur.methodStart = EncodeWithIndex(data.methodList - 1);

			if (i > 0)
			{
				uint32_t begin = 0;
				uint32_t end = 0;
				uint32_t count = 0;
				InterpreterMetadataRange::Error error = InterpreterMetadataRange::DecodeRange(
					last.fieldStart, cur.fieldStart, GetIndex(), fieldRowCount,
					std::numeric_limits<uint16_t>::max(), decoder, begin, end, count);
				RaiseInvalidMetadataRange("invalid interpreter field range", error);
				last.field_count = static_cast<uint16_t>(count);

				error = InterpreterMetadataRange::DecodeRange(
					last.methodStart, cur.methodStart, GetIndex(), methodRowCount,
					std::numeric_limits<uint16_t>::max(), decoder, begin, end, count);
				RaiseInvalidMetadataRange("invalid interpreter method range", error);
				last.method_count = static_cast<uint16_t>(count);
			}
			if (i == n - 1)
			{
				uint32_t begin = 0;
				uint32_t count = 0;
				InterpreterMetadataRange::Error error = InterpreterMetadataRange::DecodeEndpoint(
					cur.fieldStart, GetIndex(), fieldRowCount, decoder, begin);
				if (error == InterpreterMetadataRange::Error::None)
					error = InterpreterMetadataRange::CountRawRange(begin, fieldRowCount,
						std::numeric_limits<uint16_t>::max(), count);
				RaiseInvalidMetadataRange("invalid final interpreter field range", error);
				cur.field_count = static_cast<uint16_t>(count);

				error = InterpreterMetadataRange::DecodeEndpoint(
					cur.methodStart, GetIndex(), methodRowCount, decoder, begin);
				if (error == InterpreterMetadataRange::Error::None)
					error = InterpreterMetadataRange::CountRawRange(begin, methodRowCount,
						std::numeric_limits<uint16_t>::max(), count);
				RaiseInvalidMetadataRange("invalid final interpreter method range", error);
				cur.method_count = static_cast<uint16_t>(count);
			}

			if (data.extends != 0)
			{
				const Il2CppType* parentType = ReadTypeFromToken(GetGenericContainerByTypeDefinition(&cur), nullptr, DecodeTypeDefOrRefOrSpecCodedIndexTableType(data.extends), DecodeTypeDefOrRefOrSpecCodedIndexRowIndex(data.extends));

				if (parentType->type == IL2CPP_TYPE_CLASS || parentType->type == IL2CPP_TYPE_VALUETYPE)
				{
					Il2CppTypeDefinition* parentDef = (Il2CppTypeDefinition*)parentType->data.typeHandle;
					// FIXE ME . check mscorelib
					const char* parentNs = il2cpp::vm::GlobalMetadata::GetStringFromIndex(parentDef->namespaceIndex);
					if (std::strcmp(parentNs, "System") == 0)
					{
						const char* parentName = il2cpp::vm::GlobalMetadata::GetStringFromIndex(parentDef->nameIndex);
						if (std::strcmp(parentName, "Enum") == 0)
						{
							cur.bitfield |= (1 << (il2cpp::vm::kBitIsValueType - 1));
							cur.bitfield |= (1 << (il2cpp::vm::kBitIsEnum - 1));
						}
						else if (std::strcmp(parentName, "ValueType") == 0)
						{
							cur.bitfield |= (1 << (il2cpp::vm::kBitIsValueType - 1));
						}
					}
				}
				cur.parentIndex = AddIl2CppTypeCache(parentType);
			}
			else
			{
				cur.parentIndex = kInvalidIndex;
			}

			cur.elementTypeIndex = kInvalidIndex;
		}
	}

	void InterpreterImage::ComputeHasFinalizer(Il2CppTypeDefinition* def, std::vector<bool>& computFlags)
	{
		if (DecodeImageIndex(def->byvalTypeIndex) != GetIndex())
		{
			return;
		}
		uint32_t typeIndex = GetTypeRawIndex(def);
		if (computFlags[typeIndex])
		{
			return;
		}
		computFlags[typeIndex] = true;
		if (def->bitfield & (1 << (il2cpp::vm::kBitHasFinalizer - 1)))
		{
			return;
		}
		if (def->parentIndex != kInvalidIndex)
		{
			const Il2CppType* parentType = GetIl2CppTypeFromRawIndex(DecodeMetadataIndex(def->parentIndex));
			const Il2CppTypeDefinition* parentDef = metadata::GetUnderlyingTypeDefinition(parentType);
			ComputeHasFinalizer(const_cast<Il2CppTypeDefinition*>(parentDef), computFlags);
			if (parentDef->bitfield & (1 << (il2cpp::vm::kBitHasFinalizer - 1)))
			{
				def->bitfield |= (1 << (il2cpp::vm::kBitHasFinalizer - 1));
			}
		}
	}

	void InterpreterImage::InitHasFinalizers()
	{
		const Table& typeDefTb = _rawImage->GetTable(TableType::TYPEDEF);
		std::vector<bool> computFlags(typeDefTb.rowNum, false);
		for (uint32_t i = 0, n = typeDefTb.rowNum; i < n; i++)
		{
			Il2CppTypeDefinition& cur = _typesDefines[i];
			ComputeHasFinalizer(&cur, computFlags);
		}
	}

	void InterpreterImage::InitTypeDefs_2()
	{
		const Table& typeDefTb = _rawImage->GetTable(TableType::TYPEDEF);
		for (uint32_t i = 0, n = typeDefTb.rowNum; i < n; i++)
		{
			TbTypeDef data = _rawImage->ReadTypeDef(i + 1); // token from 1

			Il2CppTypeDefinition& last = _typesDefines[i > 0 ? i - 1 : 0];
			Il2CppTypeDefinition& cur = _typesDefines[i];
			uint32_t typeIndex = i; // type index start from 0, diff with field index ...

			// enum element_type == 
			if (IsEnumType(&cur))
			{
				cur.elementTypeIndex = _fieldDetails[DecodeMetadataIndex(cur.fieldStart)].fieldDef.typeIndex;
			}

			auto classLayoutRow = _classLayouts.find(typeIndex);
			uint16_t packingSize = 0;
			if (classLayoutRow != _classLayouts.end())
			{
				auto& layoutData = classLayoutRow->second;
				packingSize = layoutData.packingSize;
			}
			else
			{
				cur.bitfield |= (1 << (il2cpp::vm::kClassSizeIsDefault - 1));
			}
			if (packingSize != 0)
			{
				il2cpp::vm::PackingSize packingSizeEnum = il2cpp::vm::GlobalMetadata::ConvertPackingSizeToEnum((uint8_t)packingSize);
				cur.bitfield |= ((uint32_t)packingSizeEnum << (il2cpp::vm::kPackingSize - 1));
				cur.bitfield |= ((uint32_t)packingSizeEnum << (il2cpp::vm::kSpecifiedPackingSize - 1));
			}
			else
			{
				cur.bitfield |= (1 << (il2cpp::vm::kPackingSizeIsDefault - 1));
			}
		}
	}

	void InterpreterImage::InitParamDefs()
	{
		const Table& tb = _rawImage->GetTable(TableType::PARAM);

		// extra 16 for not name params
		_params.reserve(tb.rowNum + 16);
		_paramRawIndex2ActualParamIndex = new std::vector<TypeIndex>(tb.rowNum);
		//for (uint32_t i = 0; i < tb.rowNum; i++)
		//{
		//	uint32_t rowIndex = i + 1;
		//	Il2CppParameterDefinition& pd = _params[i].paramDef;
		//	TbParam data = _rawImage->ReadParam(rowIndex);

		//	pd.nameIndex = EncodeWithIndex(data.name);
		//	pd.token = EncodeToken(TableType::PARAM, rowIndex);
		//	// pd.typeIndex 在InitMethodDefs中解析signature后填充。
		//}
	}


	void InterpreterImage::InitFieldDefs()
	{
		const Table& fieldTb = _rawImage->GetTable(TableType::FIELD);
		_fieldDetails.resize(fieldTb.rowNum);

		for (size_t i = 0; i < _typesDefines.size(); i++)
		{
			Il2CppTypeDefinition& typeDef = _typesDefines[i];
			uint32_t start = DecodeMetadataIndex(typeDef.fieldStart);
			for (uint32_t k = 0; k < typeDef.field_count; k++)
			{
				FieldDetail& fd = _fieldDetails[start + k];
				fd.typeDefIndex = (uint32_t)i;
			}
		}

		for (uint32_t i = 0, n = fieldTb.rowNum; i < n; i++)
		{
			FieldDetail& fd = _fieldDetails[i];
			Il2CppFieldDefinition& cur = fd.fieldDef;

			fd.offset = 0;
			fd.defaultValueIndex = kDefaultValueIndexNull;

			uint32_t rowIndex = i + 1;
			TbField data = _rawImage->ReadField(rowIndex);

			BlobReader br = _rawImage->GetBlobReaderByRawIndex(data.signature);
			FieldRefSig frs;
			ReadFieldRefSig(br, GetGenericContainerByTypeDefRawIndex(DecodeMetadataIndex(fd.typeDefIndex)), frs);
			if (data.flags != 0)
			{
				Il2CppType typeWithAttrs = *frs.type;
				typeWithAttrs.attrs = data.flags;
				frs.type = MetadataPool::GetPooledIl2CppType(typeWithAttrs);
			}

			//cur = {};
			cur.nameIndex = EncodeWithIndex(data.name);
			cur.token = EncodeToken(TableType::FIELD, rowIndex);
			cur.typeIndex = AddIl2CppTypeCache(frs.type);
		}
	}

	void InterpreterImage::InitFieldLayouts()
	{
		const Table& tb = _rawImage->GetTable(TableType::FIELDLAYOUT);
		for (uint32_t i = 0; i < tb.rowNum; i++)
		{
			TbFieldLayout data = _rawImage->ReadFieldLayout(i + 1);
			_fieldDetails[data.field - 1].offset = sizeof(Il2CppObject) + data.offset;
		}
	}

	void InterpreterImage::InitFieldRVAs()
	{
		const Table& tb = _rawImage->GetTable(TableType::FIELDRVA);
		for (uint32_t i = 0; i < tb.rowNum; i++)
		{
			TbFieldRVA data = _rawImage->ReadFieldRVA(i + 1);
			FieldDetail& fd = _fieldDetails[data.field - 1];
			fd.defaultValueIndex = (uint32_t)_fieldDefaultValues.size();

			Il2CppFieldDefaultValue fdv = {};
			fdv.fieldIndex = data.field - 1;
			fdv.typeIndex = fd.fieldDef.typeIndex;

			uint32_t dataImageOffset = (uint32_t)-1;
			bool ret = _rawImage->TranslateRVAToImageOffset(data.rva, dataImageOffset);
			IL2CPP_ASSERT(ret);
#if HYBRIDCLR_UNITY_2021_OR_NEW
			fdv.dataIndex = (DefaultValueDataIndex)EncodeWithIndex(EncodeWithBlobSource(dataImageOffset, BlobSource::RAW_IMAGE));
#else
			fdv.dataIndex = (DefaultValueDataIndex)EncodeWithIndex(dataImageOffset);
#endif
			_fieldDefaultValues.push_back(fdv);
		}
	}

	void InterpreterImage::InitBlittables()
	{
		const Table& typeDefTb = _rawImage->GetTable(TableType::TYPEDEF);

		std::vector<bool> computFlags(typeDefTb.rowNum, false);

		for (uint32_t i = 0, n = typeDefTb.rowNum; i < n; i++)
		{
			ComputeBlittable(&_typesDefines[i], computFlags);
		}
	}

	void InterpreterImage::ComputeBlittable(Il2CppTypeDefinition* def, std::vector<bool>& computFlags)
	{
		if (DecodeImageIndex(def->byvalTypeIndex) != GetIndex())
		{
			return;
		}
		uint32_t typeIndex = GetTypeRawIndex(def);
		if (computFlags[typeIndex])
		{
			return;
		}
		computFlags[typeIndex] = true;

		const Il2CppType* type = GetIl2CppTypeFromRawIndex(DecodeMetadataIndex(def->byvalTypeIndex));

		const char* typeName = il2cpp::vm::GlobalMetadata::GetStringFromIndex(def->nameIndex);
		const InterpreterImageMetadataDecoder decoder;


		bool blittable = false;
		if (type->type == IL2CPP_TYPE_VALUETYPE)
		{
			blittable = true;
			for (int i = 0; i < def->field_count; i++)
			{
				uint32_t rawFieldIndex = 0;
				const InterpreterMetadataRange::Error error = InterpreterMetadataRange::ResolveOffset(
					def->fieldStart, GetIndex(), static_cast<uint32_t>(i),
					static_cast<uint32_t>(_fieldDetails.size()), decoder, rawFieldIndex);
				RaiseInvalidMetadataRange("invalid interpreter field index", error);
				const Il2CppFieldDefinition* field = GetFieldDefinitionFromRawIndex(rawFieldIndex);
				const Il2CppType* fieldType = il2cpp::vm::GlobalMetadata::GetIl2CppTypeFromIndex(field->typeIndex);
				if (!hybridclr::metadata::IsInstanceField(fieldType))
				{
					continue;
				}

				switch (fieldType->type)
				{
				case IL2CPP_TYPE_BOOLEAN:
				case IL2CPP_TYPE_CHAR:
				case IL2CPP_TYPE_I1:
				case IL2CPP_TYPE_U1:
				case IL2CPP_TYPE_I2:
				case IL2CPP_TYPE_U2:
				case IL2CPP_TYPE_I4:
				case IL2CPP_TYPE_U4:
				case IL2CPP_TYPE_I:
				case IL2CPP_TYPE_U:
				case IL2CPP_TYPE_I8:
				case IL2CPP_TYPE_U8:
				case IL2CPP_TYPE_R4:
				case IL2CPP_TYPE_R8:
				case IL2CPP_TYPE_PTR:
				case IL2CPP_TYPE_FNPTR:
				{
					break;
				}
				case IL2CPP_TYPE_VALUETYPE:
				{
					Il2CppTypeDefinition* fieldDef = (Il2CppTypeDefinition*)fieldType->data.typeHandle;
					ComputeBlittable(fieldDef, computFlags);
					blittable = fieldDef->bitfield & (1 << (il2cpp::vm::kBitIsBlittable - 1));
					break;
				}
				default:
				{
					blittable = false;
				}
				}
				if (!blittable)
				{
					break;
				}
			}
		}
		if (blittable)
		{
			def->bitfield |= (1 << (il2cpp::vm::kBitIsBlittable - 1));
		}
	}

#if HYBRIDCLR_UNITY_2021_OR_NEW
	DefaultValueDataIndex InterpreterImage::ConvertConstValue(CustomAttributeDataWriter& writer, uint32_t blobIndex, const Il2CppType* type)
	{
		Il2CppTypeEnum ttype = type->type;
		if (ttype == IL2CPP_TYPE_CLASS)
		{
			return kDefaultValueIndexNull;
		}

		DefaultValueIndex retIndex = EncodeWithIndex(EncodeWithBlobSource((DefaultValueIndex)writer.Size(), BlobSource::CONVERTED_IL2CPP_FORMAT));

		BlobReader reader = _rawImage->GetBlobReaderByRawIndex(blobIndex);
		switch (type->type)
		{
		case IL2CPP_TYPE_BOOLEAN:
		case IL2CPP_TYPE_I1:
		case IL2CPP_TYPE_U1:
		{
			writer.Write(reader, 1);
			break;
		}
		case IL2CPP_TYPE_CHAR:
		case IL2CPP_TYPE_I2:
		case IL2CPP_TYPE_U2:
		{
			writer.Write(reader, 2);
			break;
		}
		case IL2CPP_TYPE_I4:
		{
			writer.WriteCompressedInt32((int32_t)reader.Read32());
			break;
		}
		case IL2CPP_TYPE_U4:
		{
			writer.WriteCompressedUint32(reader.Read32());
			break;
		}
		case IL2CPP_TYPE_R4:
		{
			writer.Write(reader, 4);
			break;
		}
		case IL2CPP_TYPE_I8:
		case IL2CPP_TYPE_U8:
		case IL2CPP_TYPE_R8:
		{
			writer.Write(reader, 8);
			break;
		}
		case IL2CPP_TYPE_STRING:
		{
			std::string str = il2cpp::utils::StringUtils::Utf16ToUtf8((const Il2CppChar*)reader.GetData(), reader.GetLength() / 2);
			writer.WriteCompressedInt32((int32_t)str.length());
			writer.WriteBytes((const uint8_t*)str.c_str(), (int32_t)str.length());
			break;
		}
		default:
		{
			RaiseExecutionEngineException("not supported const type");
		}
		}
		
		return retIndex;
	}
#endif

	void InterpreterImage::InitConsts()
	{
		const Table& tb = _rawImage->GetTable(TableType::CONSTANT);
		for (uint32_t i = 0; i < tb.rowNum; i++)
		{
			TbConstant data = _rawImage->ReadConstant(i + 1);
			TableType parentType = DecodeHasConstantType(data.parent);
			uint32_t rowIndex = DecodeHashConstantIndex(data.parent);

			Il2CppType tempType = {};
			tempType.type = (Il2CppTypeEnum)data.type;
			const Il2CppType& type = *MetadataPool::GetPooledIl2CppType(tempType);
			TypeIndex dataTypeIndex = AddIl2CppTypeCache(&type);
#if !HYBRIDCLR_UNITY_2021_OR_NEW
			bool isNullValue = type.type == IL2CPP_TYPE_CLASS;
#endif
			switch (parentType)
			{
			case TableType::FIELD:
			{
				FieldDetail& fd = _fieldDetails[rowIndex - 1];
				fd.defaultValueIndex = (uint32_t)_fieldDefaultValues.size();

				Il2CppFieldDefaultValue fdv = {};
				fdv.fieldIndex = rowIndex - 1;
				fdv.typeIndex = dataTypeIndex;
#if HYBRIDCLR_UNITY_2021_OR_NEW
				fdv.dataIndex = ConvertConstValue(_constValues, data.value, &type);
#else
				uint32_t dataImageOffset = _rawImage->GetImageOffsetOfBlob(type.type, data.value);
				fdv.dataIndex = isNullValue ? kDefaultValueIndexNull : (DefaultValueDataIndex)EncodeWithIndex(dataImageOffset);
#endif
				_fieldDefaultValues.push_back(fdv);
				break;
			}
			case TableType::PARAM:
			{
				int32_t actualIndex = (*_paramRawIndex2ActualParamIndex)[rowIndex - 1];
				ParamDetail& fd = _params[actualIndex];
				fd.defaultValueIndex = (uint32_t)_paramDefaultValues.size();

				Il2CppParameterDefaultValue pdv = {};
				pdv.typeIndex = dataTypeIndex;
				pdv.parameterIndex = fd.parameterIndex;
#if HYBRIDCLR_UNITY_2021_OR_NEW
				pdv.dataIndex = ConvertConstValue(_constValues, data.value, &type);
#else
				uint32_t dataImageOffset = _rawImage->GetImageOffsetOfBlob(type.type, data.value);
				pdv.dataIndex = isNullValue ? kDefaultValueIndexNull : (DefaultValueDataIndex)EncodeWithIndex(dataImageOffset);
#endif
				_paramDefaultValues.push_back(pdv);
				break;
			}
			case TableType::PROPERTY:
			{
				RaiseNotSupportedException("not support property const");
				break;
			}
			default:
			{
				RaiseExecutionEngineException("not support const TableType");
				break;
			}
			}
		}
	}

	void InterpreterImage::InitCustomAttributes()
	{
		const Table& tb = _rawImage->GetTable(TableType::CUSTOMATTRIBUTE);
		_tokenCustomAttributes.reserve(tb.rowNum);


		uint32_t threadStaticMethodToken = 0;
		Il2CppCustomAttributeTypeRange* curTypeRange = nullptr;
		for (uint32_t rowIndex = 1; rowIndex <= tb.rowNum; rowIndex++)
		{
			TbCustomAttribute data = _rawImage->ReadCustomAttribute(rowIndex);
			TableType parentType = DecodeHasCustomAttributeCodedIndexTableType(data.parent);
			uint32_t parentRowIndex = DecodeHasCustomAttributeCodedIndexRowIndex(data.parent);
			uint32_t token = EncodeToken(parentType, parentRowIndex);
			if (curTypeRange == nullptr || curTypeRange->token != token)
			{
				IL2CPP_ASSERT(_tokenCustomAttributes.find(token) == _tokenCustomAttributes.end());
				int32_t attributeStartIndex = EncodeWithIndex((int32_t)_customAttribues.size());
				int32_t handleIndex = (int32_t)_customAttributeHandles.size();
				_tokenCustomAttributes[token] = { (int32_t)EncodeWithIndex(handleIndex), false, nullptr, nullptr };
#ifdef HYBRIDCLR_UNITY_2021_OR_NEW
				_customAttributeHandles.push_back({ token, (uint32_t)attributeStartIndex });
#else
				_customAttributeHandles.push_back({ token, attributeStartIndex, 0 });
#endif
				curTypeRange = &_customAttributeHandles[handleIndex];
			}
#if !HYBRIDCLR_UNITY_2021_OR_NEW
			++curTypeRange->count;
#endif
			TableType ctorMethodTableType = DecodeCustomAttributeTypeCodedIndexTableType(data.type);
			uint32_t ctorMethodRowIndex = DecodeCustomAttributeTypeCodedIndexRowIndex(data.type);
			uint32_t ctorMethodToken = EncodeToken(ctorMethodTableType, ctorMethodRowIndex);
			//CustomAttribute ca = { ctorMethodToken, data.value };
			//ca.value = data.value;
			//ReadMethodRefInfoFromToken(nullptr, nullptr, , ca.attrCtorMethod);
			_customAttribues.push_back({ ctorMethodToken, data.value });

			if (parentType == TableType::FIELD)
			{
				// try set thread static flags
				if (threadStaticMethodToken == 0)
				{
					if (IsThreadStaticCtorToken(ctorMethodTableType, ctorMethodRowIndex))
					{
						threadStaticMethodToken = ctorMethodToken;
					}
				}
				if (ctorMethodToken == threadStaticMethodToken)
				{
					IL2CPP_ASSERT(threadStaticMethodToken != 0);
					_fieldDetails[parentRowIndex - 1].offset = THREAD_LOCAL_STATIC_MASK;
				}
			}

		}
		IL2CPP_ASSERT(_tokenCustomAttributes.size() == _customAttributeHandles.size());
#ifdef HYBRIDCLR_UNITY_2021_OR_NEW
		// add extra Il2CppCustomAttributeTypeRange for compute count
		_customAttributeHandles.push_back({ 0, EncodeWithIndex((int32_t)_customAttribues.size()) });
#endif
#if !HYBRIDCLR_UNITY_2022_OR_NEW
		_customAttribtesCaches.resize(_tokenCustomAttributes.size());
#endif
	}

#ifdef HYBRIDCLR_UNITY_2021_OR_NEW

	void InterpreterImage::InitCustomAttributeData(CustomAttributesInfo& cai, const Il2CppCustomAttributeTypeRange& dataRange)
	{
		// Resolution also accesses shared signature/type pools. Preserve the
		// original recursive conversion-wide lock; local buffers still permit
		// safe same-thread nested conversion without consuming owner indices.
		il2cpp::os::FastAutoLock metaLock(&il2cpp::vm::g_MetadataLock);
		if (cai.inited)
			return;
		BuildCustomAttributesData(cai, dataRange);
	}

	void InterpreterImage::BuildCustomAttributesData(CustomAttributesInfo& cai, const Il2CppCustomAttributeTypeRange& curTypeRange)
	{
		hybridclr::interpreter::ExecutingInterpImageScope scope(hybridclr::interpreter::InterpreterModule::GetCurrentThreadMachineState(), this->_il2cppImage);
		const Il2CppCustomAttributeDataRange& nextTypeRange = *(&curTypeRange + 1);
		uint32_t attrStart = 0;
		uint32_t attrEnd = 0;
		uint32_t attrCount = 0;
		const InterpreterImageMetadataDecoder decoder;
		const InterpreterMetadataRange::Error rangeError = InterpreterMetadataRange::DecodeRange(
			curTypeRange.startOffset, nextTypeRange.startOffset, GetIndex(),
			static_cast<uint32_t>(_customAttribues.size()),
			std::numeric_limits<uint32_t>::max(), decoder, attrStart, attrEnd, attrCount);
		RaiseInvalidMetadataRange("invalid interpreter custom attribute range", rangeError);
		uint32_t methodIndexBytes = 0;
		const InterpreterMetadataRange::Error layoutError = InterpreterMetadataRange::ValidateCustomAttributeLayout(
			attrStart, attrEnd, attrCount, 0, methodIndexBytes);
		RaiseInvalidMetadataRange("invalid interpreter custom attribute layout", layoutError);
		CustomAttributeDataWriter output(1024);
		CustomAttributeTypeIndexBatch typeBatch;
		output.WriteAttributeCount(attrCount);
		int32_t attrStartOffset = static_cast<int32_t>(attrStart);
		int32_t methodIndexDataOffset = output.Size();
		output.Skip(static_cast<int32_t>(methodIndexBytes));
		for (uint32_t i = 0; i < attrCount; i++)
		{
			const CustomAttribute& ca = _customAttribues[attrStartOffset + (int32_t)i];
			MethodRefInfo mri = {};
			ReadMethodRefInfoFromToken(nullptr, nullptr, DecodeTokenTableType(ca.ctorMethodToken), DecodeTokenRowIndex(ca.ctorMethodToken), mri);
			const MethodInfo* ctorMethod = GetMethodInfoFromMethodDef(mri.containerType, mri.methodDef);
			MethodIndex ctorIndex = il2cpp::vm::GlobalMetadata::GetMethodIndexFromDefinition(mri.methodDef);
			output.WriteMethodIndex(methodIndexDataOffset, ctorIndex);
			methodIndexDataOffset += sizeof(int32_t);
			if (ca.value != 0)
			{
				BlobReader reader = _rawImage->GetBlobReaderByRawIndex(ca.value);
				ConvertILCustomAttributeData2Il2CppFormat(output, typeBatch, ctorMethod, reader);
			}
			else
			{
				IL2CPP_ASSERT(mri.methodDef->parameterCount == 0);
				output.WriteCompressedUint32(0);
				output.WriteCompressedUint32(0);
				output.WriteCompressedUint32(0);
			}
		}

		// Nested reflection can initialize this exact range while the local
		// conversion is resolving.  Its completed publication wins.
		il2cpp::os::FastAutoLock metaLock(&il2cpp::vm::g_MetadataLock);
		if (cai.inited)
			return;

		const std::vector<const Il2CppType*>& stableTypes = typeBatch.Types();
		const uint64_t rawLimit = uint64_t(1) << 31;
		if (_types.size() > rawLimit || stableTypes.size() > rawLimit - _types.size())
			RaiseExecutionEngineException("custom attribute type cache index overflow");
		const uint64_t requiredSize = uint64_t(_types.size()) + stableTypes.size();
		if (requiredSize > _types.max_size())
			RaiseExecutionEngineException("custom attribute type cache exceeds vector limit");
		const uint32_t firstTypeIndex = static_cast<uint32_t>(_types.size());
		for (const CustomAttributeTypeIndexBatch::Fixup& fixup : typeBatch.Fixups())
		{
			if (fixup.writer != &output || fixup.typeIndex >= stableTypes.size())
				RaiseExecutionEngineException("invalid or unrelocated custom attribute type fixup");
			uint32_t encodedIndex = EncodeWithIndex(firstTypeIndex + fixup.typeIndex);
			output.PatchCompressedInt32Placeholder(fixup.outputOffset, (int32_t)encodedIndex);
		}

		if (requiredSize > _types.capacity())
		{
			const uint64_t limit = std::min(rawLimit, static_cast<uint64_t>(_types.max_size()));
			const uint64_t grown = std::min(limit, uint64_t(_types.capacity()) + _types.capacity() / 2 + 1);
			_types.reserve(static_cast<size_t>(std::max(requiredSize, grown)));
		}
		void* resultData = HYBRIDCLR_MALLOC(output.Size());
		if (resultData == nullptr)
			il2cpp::vm::Exception::RaiseOutOfMemoryException();
		std::memcpy(resultData, output.Data(), output.Size());
		typeBatch.ReleaseOwnership();
		for (const Il2CppType* type : stableTypes)
			_types.push_back(type);

		cai.dataStartPtr = resultData;
		cai.dataEndPtr = (uint8_t*)resultData + output.Size();
		il2cpp::os::Atomic::FullMemoryBarrier();
		cai.inited = true;
	}

	void InterpreterImage::WriteEncodeTypeEnum(CustomAttributeDataWriter& writer, CustomAttributeTypeIndexBatch& typeBatch, const Il2CppType* type)
	{
		Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(type);
		if (type->type == IL2CPP_TYPE_ENUM || klass->enumtype)
		{
			writer.WriteByte((byte)IL2CPP_TYPE_ENUM);
			if (type->type == IL2CPP_TYPE_CLASS || type->type == IL2CPP_TYPE_VALUETYPE)
				writer.WriteCompressedInt32(((Il2CppTypeDefinition*)type->data.typeHandle)->byvalTypeIndex);
			else
				typeBatch.WriteTypeIndex(writer, type);
		}
		else if (klass == il2cpp_defaults.systemtype_class)
		{
			writer.WriteByte((byte)IL2CPP_TYPE_IL2CPP_TYPE_INDEX);
		}
		else
		{
			writer.WriteByte((uint8_t)type->type);
		}
	}

	void InterpreterImage::ConvertBoxedValue(CustomAttributeDataWriter& writer, CustomAttributeTypeIndexBatch& typeBatch, BlobReader& reader, bool writeType)
	{
		uint64_t obj = 0;
		Il2CppType kind = {};
		ReadCustomAttributeFieldOrPropType(reader, kind);
		ConvertFixedArg(writer, typeBatch, reader, &kind, true);
	}

	void InterpreterImage::ConvertSystemType(CustomAttributeDataWriter& writer, CustomAttributeTypeIndexBatch& typeBatch, BlobReader& reader, bool writeType)
	{
		if (writeType)
		{
			writer.WriteByte((byte)IL2CPP_TYPE_IL2CPP_TYPE_INDEX);
		}
		Il2CppString* fullName = ReadSerString(reader);
		if (!fullName)
		{
			writer.WriteCompressedInt32(-1);
			return;
		}
		Il2CppReflectionType* type = GetReflectionTypeFromName(fullName);
		if (!type)
		{
			std::string stdTypeName = il2cpp::utils::StringUtils::Utf16ToUtf8(fullName->chars);
			TEMP_FORMAT(errMsg, "CustomAttribute fixed arg type:System.Type fullName:'%s' not find", stdTypeName.c_str());
			il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetTypeLoadException(errMsg));
		}
		Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(type->type);
		if (!klass->generic_class && (Il2CppTypeDefinition*)klass->typeMetadataHandle)
		{
			writer.WriteCompressedInt32(((Il2CppTypeDefinition*)klass->typeMetadataHandle)->byvalTypeIndex);
		}
		else
		{
			typeBatch.WriteTypeIndex(writer, type->type);
		}
	}

	void InterpreterImage::ConvertFixedArg(CustomAttributeDataWriter& writer, CustomAttributeTypeIndexBatch& typeBatch, BlobReader& reader, const Il2CppType* type, bool writeType)
	{
		switch (type->type)
		{
		case IL2CPP_TYPE_BOOLEAN:
		case IL2CPP_TYPE_I1:
		case IL2CPP_TYPE_U1:
		{
			if (writeType)
			{
				writer.WriteByte((uint8_t)type->type);
			}
			writer.Write(reader, 1);
			break;
		}
		case IL2CPP_TYPE_CHAR:
		case IL2CPP_TYPE_I2:
		case IL2CPP_TYPE_U2:
		{
			if (writeType)
			{
				writer.WriteByte((uint8_t)type->type);
			}
			writer.Write(reader, 2);
			break;
		}
		case IL2CPP_TYPE_I4:
		{
			if (writeType)
			{
				writer.WriteByte((uint8_t)type->type);
			}
			writer.WriteCompressedInt32((int32_t)reader.Read32());
			break;
		}
		case IL2CPP_TYPE_U4:
		{
			if (writeType)
			{
				writer.WriteByte((uint8_t)type->type);
			}
			writer.WriteCompressedUint32(reader.Read32());
			break;
		}
		case IL2CPP_TYPE_R4:
		{
			if (writeType)
			{
				writer.WriteByte((uint8_t)type->type);
			}
			writer.Write(reader, 4);
			break;
		}
		case IL2CPP_TYPE_I8:
		case IL2CPP_TYPE_U8:
		case IL2CPP_TYPE_R8:
		{
			if (writeType)
			{
				writer.WriteByte((uint8_t)type->type);
			}
			writer.Write(reader, 8);
			break;
		}
		case IL2CPP_TYPE_SZARRAY:
		{
			if (writeType)
			{
				writer.WriteByte((uint8_t)type->type);
			}
			int32_t numElem = (int32_t)reader.Read32();
			if (numElem < -1)
				RaiseBadImageException("invalid custom attribute array length");
			writer.WriteCompressedInt32(numElem);
			if (numElem != -1)
			{
				//Il2CppType kind = {};
				//ReadCustomAttributeFieldOrPropType(reader, kind);
				const Il2CppType* eleType = type->data.type;
				WriteEncodeTypeEnum(writer, typeBatch, eleType);
				if (eleType->type == IL2CPP_TYPE_OBJECT)
				{
					// kArrayTypeWithDifferentElements
					writer.WriteByte(1);
					for (int32_t i = 0; i < numElem; i++)
					{
						ConvertBoxedValue(writer, typeBatch, reader, false);
					}
				}
				else
				{
					// all element type is same.
					writer.WriteByte(0);
					for (int32_t i = 0; i < numElem; i++)
					{
						ConvertFixedArg(writer, typeBatch, reader, eleType, false);
					}
				}

			}
			break;
		}
		case IL2CPP_TYPE_STRING:
		{
			if (writeType)
			{
				writer.WriteByte((uint8_t)type->type);
			}
			byte b = reader.PeekByte();
			if (b == 0xFF)
			{
				reader.SkipByte();
				writer.WriteCompressedInt32(-1);
			}
			else if (b == 0)
			{
				reader.SkipByte();
				writer.WriteCompressedInt32(0);
			}
			else
			{
				uint32_t len = reader.ReadCompressedUint32();
				if (len > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
					RaiseBadImageException("custom attribute string exceeds signed length domain");
				writer.WriteCompressedInt32((int32_t)len);
				writer.Write(reader, static_cast<int32_t>(len));
			}
			break;
		}
		case IL2CPP_TYPE_OBJECT:
		{
			ConvertBoxedValue(writer, typeBatch, reader, writeType);
			break;
		}
		case IL2CPP_TYPE_CLASS:
		{
			Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(type);
			if (!klass)
			{
				RaiseExecutionEngineException("type not find");
			}
			if (klass == il2cpp_defaults.object_class)
			{
				ConvertBoxedValue(writer, typeBatch, reader, writeType);
			}
			else if (klass == il2cpp_defaults.systemtype_class)
			{
				ConvertSystemType(writer, typeBatch, reader, writeType);
			}
			else
			{
				TEMP_FORMAT(errMsg, "fixed arg type:%s.%s not support", klass->namespaze, klass->name);
				RaiseNotSupportedException(errMsg);
			}
			break;
		}
		case IL2CPP_TYPE_VALUETYPE:
		{
			Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(type);
			if (writeType)
			{
				writer.WriteByte((byte)IL2CPP_TYPE_ENUM);
				IL2CPP_ASSERT(klass->enumtype);
				if (klass->generic_class)
					typeBatch.WriteTypeIndex(writer, type);
				else
					writer.WriteCompressedInt32(((Il2CppTypeDefinition*)type->data.typeHandle)->byvalTypeIndex);
			}
			ConvertFixedArg(writer, typeBatch, reader, &klass->element_class->byval_arg, false);
			break;
		}
		case IL2CPP_TYPE_SYSTEM_TYPE:
		{
			ConvertSystemType(writer, typeBatch, reader, true);
			break;
		}
		case IL2CPP_TYPE_BOXED_OBJECT:
		{
			uint8_t fieldOrPropType = reader.ReadByte();
			IL2CPP_ASSERT(fieldOrPropType == 0x51);
			ConvertBoxedValue(writer, typeBatch, reader, writeType);
			break;
		}
		case IL2CPP_TYPE_ENUM:
		{
			Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(type);
			IL2CPP_ASSERT(klass->enumtype);
			if (writeType)
			{
				if (klass->generic_class)
					typeBatch.WriteTypeIndex(writer, type);
				else
					writer.WriteCompressedInt32(((Il2CppTypeDefinition*)type->data.typeHandle)->byvalTypeIndex);
			}
			ConvertFixedArg(writer, typeBatch, reader, &klass->element_class->byval_arg, false);
			break;
		}
		default:
		{
			RaiseExecutionEngineException("not support fixed argument type");
		}
		}
	}

	void InterpreterImage::GetFieldDeclaringTypeIndexAndFieldIndexByName(const Il2CppTypeDefinition* declaringType, const char* name, int32_t& typeIndex, int32_t& fieldIndex)
	{
		Il2CppClass* klass = il2cpp::vm::GlobalMetadata::GetTypeInfoFromHandle((Il2CppMetadataTypeHandle)declaringType);
		FieldInfo* field = il2cpp::vm::Class::GetFieldFromName(klass, name);
		if (!field)
		{
			RaiseExecutionEngineException("GetFieldDeclaringTypeIndexAndFieldIndexByName can't find field");
		}
		if (field->parent == klass)
		{
			typeIndex = kTypeDefinitionIndexInvalid;
		}
		else
		{
			klass = field->parent;
			if (klass->generic_class)
			{
				RaiseExecutionEngineException("GetFieldDeclaringTypeIndexAndFieldIndexByName doesn't support field of generic CustomAttribute");
			}
			typeIndex = il2cpp::vm::GlobalMetadata::GetIndexForTypeDefinition(klass);
		}
		fieldIndex = (int32_t)(field - klass->fields);
	}

	void InterpreterImage::GetPropertyDeclaringTypeIndexAndPropertyIndexByName(const Il2CppTypeDefinition* declaringType, const char* name, int32_t& typeIndex, int32_t& fieldIndex)
	{
		Il2CppClass* klass = il2cpp::vm::GlobalMetadata::GetTypeInfoFromHandle((Il2CppMetadataTypeHandle)declaringType);
		const PropertyInfo* propertyInfo = il2cpp::vm::Class::GetPropertyFromName(klass, name);
		if (!propertyInfo)
		{
			RaiseExecutionEngineException("GetFieldDeclaringTypeIndexAndFieldIndexByName can't find field");
		}
		if (propertyInfo->parent == klass)
		{
			typeIndex = kTypeDefinitionIndexInvalid;
		}
		else
		{
			klass = propertyInfo->parent;
			if (klass->generic_class)
			{
				RaiseExecutionEngineException("GetPropertyDeclaringTypeIndexAndPropertyIndexByName doesn't support field of generic CustomAttribute");
			}
			typeIndex = il2cpp::vm::GlobalMetadata::GetIndexForTypeDefinition(klass);
		}
#if UNITY_ENGINE_TUANJIE
		fieldIndex = -1;
		for (int32_t i = 0; i < klass->property_count; i++)
		{
			if (klass->properties[i] == propertyInfo)
			{
				fieldIndex = i;
				break;;
			}
		}
		IL2CPP_ASSERT(fieldIndex != -1);
#else
		fieldIndex = (int32_t)(propertyInfo - klass->properties);
#endif
	}

	void InterpreterImage::ConvertILCustomAttributeData2Il2CppFormat(CustomAttributeDataWriter& output, CustomAttributeTypeIndexBatch& typeBatch, const MethodInfo* ctorMethod, BlobReader& reader)
	{
		uint16_t prolog = reader.Read16();
		IL2CPP_ASSERT(prolog == 0x0001);
		IL2CPP_ASSERT(!ctorMethod->is_generic);

		CustomAttributeDataWriter ctorArgBlob(256);
		for (uint16_t i = 0; i < ctorMethod->parameters_count; i++)
		{
			const Il2CppType* paramType = GET_METHOD_PARAMETER_TYPE(ctorMethod->parameters[i]);
			ConvertFixedArg(ctorArgBlob, typeBatch, reader, paramType, true);
		}

		uint16_t numNamed = reader.Read16();

		uint32_t fieldCount = 0;
		uint32_t propertyCount = 0;
		CustomAttributeDataWriter fieldBlob(256);
		CustomAttributeDataWriter propertyBlob(256);
		const Il2CppTypeDefinition* declaringType = GetUnderlyingTypeDefinition(&ctorMethod->klass->byval_arg);
		for (uint16_t idx = 0; idx < numNamed; idx++)
		{
			byte fieldOrPropTypeTag = reader.ReadByte();
			IL2CPP_ASSERT(fieldOrPropTypeTag == 0x53 || fieldOrPropTypeTag == 0x54);
			Il2CppType fieldOrPropType = {};
			ReadCustomAttributeFieldOrPropType(reader, fieldOrPropType);
			Il2CppString* fieldOrPropName = ReadSerString(reader);
			std::string stdStrName = il2cpp::utils::StringUtils::Utf16ToUtf8(fieldOrPropName->chars);
			const char* cstrName = stdStrName.c_str();
			int32_t fieldOrPropertyDeclaringTypeIndex = kTypeIndexInvalid;
			int32_t fieldOrPropertyIndex = 0;
			if (fieldOrPropTypeTag == 0x53)
			{
				++fieldCount;
				ConvertFixedArg(fieldBlob, typeBatch, reader, &fieldOrPropType, true);
				GetFieldDeclaringTypeIndexAndFieldIndexByName(declaringType, cstrName, fieldOrPropertyDeclaringTypeIndex, fieldOrPropertyIndex);
				if (fieldOrPropertyDeclaringTypeIndex == kTypeDefinitionIndexInvalid)
				{
					fieldBlob.WriteCompressedInt32(fieldOrPropertyIndex);
				}
				else
				{
					fieldBlob.WriteCompressedInt32(-fieldOrPropertyIndex - 1);
					fieldBlob.WriteCompressedUint32(fieldOrPropertyDeclaringTypeIndex);
				}
			}
			else
			{
				++propertyCount;
				ConvertFixedArg(propertyBlob, typeBatch, reader, &fieldOrPropType, true);
				GetPropertyDeclaringTypeIndexAndPropertyIndexByName(declaringType, cstrName, fieldOrPropertyDeclaringTypeIndex, fieldOrPropertyIndex);
				if (fieldOrPropertyDeclaringTypeIndex == kTypeDefinitionIndexInvalid)
				{
					propertyBlob.WriteCompressedInt32(fieldOrPropertyIndex);
				}
				else
				{
					propertyBlob.WriteCompressedInt32(-fieldOrPropertyIndex - 1);
					propertyBlob.WriteCompressedUint32(fieldOrPropertyDeclaringTypeIndex);
				}
			}
		}
		output.WriteCompressedUint32(ctorMethod->parameters_count);
		output.WriteCompressedUint32(fieldCount);
		output.WriteCompressedUint32(propertyCount);
		typeBatch.AppendTo(output, ctorArgBlob);
		typeBatch.AppendTo(output, fieldBlob);
		typeBatch.AppendTo(output, propertyBlob);
	}
#endif

#if !HYBRIDCLR_UNITY_2021_OR_NEW

	void InterpreterImage::ConstructCustomAttribute(BlobReader& reader, Il2CppObject* obj, const MethodInfo* ctorMethod)
	{
		uint16_t prolog = reader.Read16();
		IL2CPP_ASSERT(prolog == 0x0001);
		if (ctorMethod->parameters_count == 0)
		{
			il2cpp::vm::Runtime::Invoke(ctorMethod, obj, nullptr, nullptr);
		}
		else
		{
			int32_t argSize = sizeof(uint64_t) * ctorMethod->parameters_count;
			uint64_t* argDatas = (uint64_t*)alloca(argSize);
			std::memset(argDatas, 0, argSize);
			void** argPtrs = (void**)alloca(sizeof(void*) * ctorMethod->parameters_count); // same with argDatas
			for (uint8_t i = 0; i < ctorMethod->parameters_count; i++)
			{
				argPtrs[i] = argDatas + i;
				const Il2CppType* paramType = GET_METHOD_PARAMETER_TYPE(ctorMethod->parameters[i]);
				ReadFixedArg(reader, paramType, argDatas + i);
				Il2CppClass* paramKlass = il2cpp::vm::Class::FromIl2CppType(paramType);
				if (!IS_CLASS_VALUE_TYPE(paramKlass))
				{
					argPtrs[i] = (void*)argDatas[i];
				}
			}
			il2cpp::vm::Runtime::Invoke(ctorMethod, obj, argPtrs, nullptr);
			// clear ref. may not need. gc memory barrier
			std::memset(argDatas, 0, argSize);
		}
		uint16_t numNamed = reader.Read16();
		Il2CppClass* klass = obj->klass;
		for (uint16_t idx = 0; idx < numNamed; idx++)
		{
			byte fieldOrPropTypeTag = reader.ReadByte();
			IL2CPP_ASSERT(fieldOrPropTypeTag == 0x53 || fieldOrPropTypeTag == 0x54);
			Il2CppType fieldOrPropType = {};
			ReadCustomAttributeFieldOrPropType(reader, fieldOrPropType);
			Il2CppString* fieldOrPropName = ReadSerString(reader);
			std::string stdStrName = il2cpp::utils::StringUtils::Utf16ToUtf8(fieldOrPropName->chars);
			const char* cstrName = stdStrName.c_str();
			uint64_t value = 0;
			ReadFixedArg(reader, &fieldOrPropType, &value);
			if (fieldOrPropTypeTag == 0x53)
			{
				FieldInfo* field = il2cpp::vm::Class::GetFieldFromName(klass, cstrName);
				if (!field)
				{
					TEMP_FORMAT(errMsg, "CustomAttribute field missing. klass:%s.%s field:%s", klass->namespaze, klass->name, cstrName);
					il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetTypeInitializationException(errMsg, nullptr));
				}
				IL2CPP_ASSERT(IsTypeEqual(&fieldOrPropType, field->type));
				il2cpp::vm::Field::SetValue(obj, field, &value);
			}
			else
			{
				const PropertyInfo* prop = il2cpp::vm::Class::GetPropertyFromName(klass, cstrName);
				if (!prop)
				{
					TEMP_FORMAT(errMsg, "CustomAttribute property missing. klass:%s property:%s", klass->name, cstrName);
					il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetTypeInitializationException(errMsg, nullptr));
				}
				IL2CPP_ASSERT(IsTypeEqual(&fieldOrPropType, GET_METHOD_PARAMETER_TYPE(prop->set->parameters[0])));
				Il2CppException* ex = nullptr;
				Il2CppClass* propKlass = il2cpp::vm::Class::FromIl2CppType(&fieldOrPropType);
				IL2CPP_ASSERT(propKlass);
				void* args[] = { (IS_CLASS_VALUE_TYPE(propKlass) ? &value : (void*)value) };
				il2cpp::vm::Runtime::Invoke(prop->set, obj, args, &ex);
				if (ex)
				{
					il2cpp::vm::Exception::Raise(ex);
				}
			}
		}
	}

	CustomAttributesCache* InterpreterImage::GenerateCustomAttributesCacheInternal(CustomAttributeIndex index)
	{
		IL2CPP_ASSERT(index != kCustomAttributeIndexInvalid);
		IL2CPP_ASSERT(index < (CustomAttributeIndex)_customAttributeHandles.size());
		CustomAttributesCache* cache = _customAttribtesCaches[index];
		if (cache)
		{
			return cache;
		}

		Il2CppCustomAttributeTypeRange& typeRange = _customAttributeHandles[index];

		hybridclr::interpreter::ExecutingInterpImageScope scope(hybridclr::interpreter::InterpreterModule::GetCurrentThreadMachineState(), this->_il2cppImage);

		cache = (CustomAttributesCache*)IL2CPP_CALLOC(1, sizeof(CustomAttributesCache));
		int32_t count= (int32_t)typeRange.count;
		cache->count = count;
		cache->attributes = (Il2CppObject**)il2cpp::gc::GarbageCollector::AllocateFixed(sizeof(Il2CppObject*) * count, 0);

		int32_t start = DecodeMetadataIndex(GET_CUSTOM_ATTRIBUTE_TYPE_RANGE_START(typeRange));
		for (int32_t i = 0; i < count; i++)
		{
			int32_t attrIndex = start + i;
			IL2CPP_ASSERT(attrIndex >= 0 && attrIndex < (int32_t)_customAttribues.size());
			CustomAttribute& ca = _customAttribues[attrIndex];
			MethodRefInfo mri = {};
			ReadMethodRefInfoFromToken(nullptr, nullptr, DecodeTokenTableType(ca.ctorMethodToken), DecodeTokenRowIndex(ca.ctorMethodToken), mri);
			const MethodInfo* ctorMethod = GetMethodInfoFromMethodDef(mri.containerType, mri.methodDef);
			IL2CPP_ASSERT(ctorMethod);
			Il2CppClass* klass = ctorMethod->klass;
			Il2CppObject* attr = il2cpp::vm::Object::New(klass);
			Il2CppArray* paramArr = nullptr;
			if (ca.value != 0)
			{
				BlobReader reader = _rawImage->GetBlobReaderByRawIndex(ca.value);
				ConstructCustomAttribute(reader, attr, ctorMethod);
			}
			else
			{
				IL2CPP_ASSERT(ctorMethod->parameters_count == 0);
				il2cpp::vm::Runtime::Invoke(ctorMethod, attr, nullptr, nullptr);
			}

			cache->attributes[i] = attr;
			HYBRIDCLR_SET_WRITE_BARRIER((void**)cache->attributes + i);
		}

		il2cpp::os::FastAutoLock metaLock(&il2cpp::vm::g_MetadataLock);
		CustomAttributesCache* original = _customAttribtesCaches[index];
		if (original)
		{
			// A non-NULL return value indicates some other thread already generated this cache.
			// We need to cleanup the resources we allocated
			il2cpp::gc::GarbageCollector::FreeFixed(cache->attributes);
			HYBRIDCLR_FREE(cache);
			return original;
		}
		il2cpp::os::Atomic::FullMemoryBarrier();
		_customAttribtesCaches[index] = cache;
		return cache;
	}
#elif HYBRIDCLR_UNITY_2021
	CustomAttributesCache* InterpreterImage::GenerateCustomAttributesCacheInternal(CustomAttributeIndex index)
	{
		IL2CPP_ASSERT(index != kCustomAttributeIndexInvalid);
		IL2CPP_ASSERT(index < (CustomAttributeIndex)_customAttributeHandles.size());
		CustomAttributesCache* cache = _customAttribtesCaches[index];
		if (cache)
		{
			return cache;
		}

		hybridclr::interpreter::ExecutingInterpImageScope scope(hybridclr::interpreter::InterpreterModule::GetCurrentThreadMachineState(), this->_il2cppImage);

		Il2CppCustomAttributeTypeRange& typeRange = _customAttributeHandles[index];
		void* start;
		void* end;
		std::tie(start, end) = CreateCustomAttributeDataTuple(&typeRange);
		IL2CPP_ASSERT(start && end);

		il2cpp::metadata::CustomAttributeDataReader reader(start, end);

		cache = (CustomAttributesCache*)IL2CPP_CALLOC(1, sizeof(CustomAttributesCache));
		cache->count = (int)reader.GetCount();
		cache->attributes = (Il2CppObject**)il2cpp::gc::GarbageCollector::AllocateFixed(sizeof(Il2CppObject*) * cache->count, 0);

		il2cpp::metadata::CustomAttributeDataIterator iter = reader.GetDataIterator();
		for (int i = 0; i < cache->count; i++)
		{
			Il2CppException* exc = NULL;
			il2cpp::metadata::CustomAttributeCreator creator;
			if (reader.VisitCustomAttributeData(_il2cppImage, &iter, &creator, &exc))
			{
				cache->attributes[i] = creator.GetAttribute(&exc);
				HYBRIDCLR_SET_WRITE_BARRIER((void**)&cache->attributes[i]);
			}

			if (exc != NULL)
			{
				il2cpp::gc::GarbageCollector::FreeFixed(cache->attributes);
				HYBRIDCLR_FREE(cache);
				il2cpp::vm::Exception::Raise(exc);
			}
		}


		il2cpp::os::FastAutoLock metaLock(&il2cpp::vm::g_MetadataLock);
		CustomAttributesCache* original = _customAttribtesCaches[index];
		if (original)
		{
			// A non-NULL return value indicates some other thread already generated this cache.
			// We need to cleanup the resources we allocated
			il2cpp::gc::GarbageCollector::FreeFixed(cache->attributes);
			HYBRIDCLR_FREE(cache);
			return original;
		}
		il2cpp::os::Atomic::FullMemoryBarrier();
		_customAttribtesCaches[index] = cache;
		return cache;
	}
#endif

	void InterpreterImage::InitModuleRefs()
	{
		const Table& moduleRefTb = _rawImage->GetTable(TableType::MODULEREF);
		_moduleRefs.reserve(moduleRefTb.rowNum);
		for (uint32_t rid = 1; rid <= moduleRefTb.rowNum; rid++)
		{
			TbModuleRef moduleRef = _rawImage->ReadModuleRef(rid);
			const char* moduleName = _rawImage->GetStringFromRawIndex(moduleRef.name);
			_moduleRefs.push_back(moduleName);
		}
	}

	void InterpreterImage::InitImplMaps()
	{
		const Table& implMapTb = _rawImage->GetTable(TableType::IMPLMAP);
		_implMapInfos.reserve(implMapTb.rowNum);
		for (uint32_t rid = 1; rid <= implMapTb.rowNum; rid++)
		{
			TbImplMap implMap = _rawImage->ReadImplMap(rid);
			ImplMapInfo info = {};
			info.moduleName = _moduleRefs[DecodeTokenRowIndex(implMap.importScope) - 1];
			info.importName = _rawImage->GetStringFromRawIndex(implMap.importName);
			info.mappingFlags = implMap.mappingFlags;
			uint32_t memberForwardedToken = hybridclr::metadata::ConvertMemberForwardedToken2Token(implMap.memberForwarded);
			_implMapInfos.insert({ memberForwardedToken, info });
		}
	}

	void InterpreterImage::InitMethodDefs0()
	{
		const Table& typeDefTb = _rawImage->GetTable(TableType::TYPEDEF);
		const Table& methodTb = _rawImage->GetTable(TableType::METHOD);

		_methodDefines.resize(methodTb.rowNum);
		for (Il2CppMethodDefinition& md : _methodDefines)
		{
			md.genericContainerIndex = kGenericContainerIndexInvalid;
		}
	}

	void InterpreterImage::InitMethodDefs()
	{
		const Table& typeDefTb = _rawImage->GetTable(TableType::TYPEDEF);
		const Table& methodTb = _rawImage->GetTable(TableType::METHOD);

		for (uint32_t i = 0, n = typeDefTb.rowNum; i < n; i++)
		{
			Il2CppTypeDefinition& typeDef = _typesDefines[i];
			uint32_t rawMethodStart = DecodeMetadataIndex(typeDef.methodStart);

			for (int m = 0; m < typeDef.method_count; m++)
			{
				Il2CppMethodDefinition& md = _methodDefines[rawMethodStart + m];
				md.declaringType = EncodeWithIndex(i);
			}
		}

		int32_t paramTableRowNum = _rawImage->GetTable(TableType::PARAM).rowNum;
		for (uint32_t index = 0; index < methodTb.rowNum; index++)
		{
			Il2CppMethodDefinition& md = _methodDefines[index];
			uint32_t rowIndex = index + 1;
			TbMethod methodData = _rawImage->ReadMethod(rowIndex);

			md.nameIndex = EncodeWithIndex(methodData.name);
			md.parameterStart = methodData.paramList - 1;
			//md.genericContainerIndex = kGenericContainerIndexInvalid;
			md.token = EncodeToken(TableType::METHOD, rowIndex);
			md.flags = methodData.flags;
			md.iflags = methodData.implFlags;
			md.slot = kInvalidIl2CppMethodSlot;
			if (index > 0)
			{
				auto& last = _methodDefines[index - 1];
				last.parameterCount = md.parameterStart - last.parameterStart;
			}
			if (index == methodTb.rowNum - 1)
			{
				md.parameterCount = (int)paramTableRowNum - (int32_t)md.parameterStart;
			}

			//MethodBody& body = _methodBodies[index];
			//ReadMethodBody(md, methodData, body);
		}

		for (uint32_t i = 0, n = typeDefTb.rowNum; i < n; i++)
		{
			Il2CppTypeDefinition& typeDef = _typesDefines[i];
			uint32_t rawMethodStart = DecodeMetadataIndex(typeDef.methodStart);
			bool isInterface = IsInterface(typeDef.flags);
			uint16_t slotIdx = 0;
			for (int m = 0; m < typeDef.method_count; m++)
			{
				Il2CppMethodDefinition& md = _methodDefines[rawMethodStart + m];
				const char* methodName = _rawImage->GetStringFromRawIndex(DecodeMetadataIndex(md.nameIndex));
				if (!std::strcmp(methodName, ".cctor"))
				{
					typeDef.bitfield |= (1 << (il2cpp::vm::kBitHasStaticConstructor - 1));
				}
				if (!std::strcmp(methodName, "Finalize"))
				{
					typeDef.bitfield |= (1 << (il2cpp::vm::kBitHasFinalizer - 1));
				}
				if (isInterface && IsInstanceMethod(&md) && IsVirtualMethod(md.flags))
				{
					md.slot = slotIdx++;
				}
				// TODO 可以考虑优化一下,将 signature在前一步存到暂时不用的 returnType里
				TbMethod methodData = _rawImage->ReadMethod(rawMethodStart + m + 1);

				BlobReader methodSigReader = _rawImage->GetBlobReaderByRawIndex(methodData.signature);
				uint32_t namedParamStart = md.parameterStart;
				uint32_t namedParamCount = md.parameterCount;

				uint32_t actualParamStart = (uint32_t)_params.size();
				ReadMethodDefSig(
					methodSigReader,
					GetGenericContainerByTypeDefinition(&typeDef),
					GetGenericContainerByRawIndex(DecodeMetadataIndex(md.genericContainerIndex)),
					md,
					_params);
				uint32_t actualParamCount = (uint32_t)_params.size() - actualParamStart;
				md.parameterStart = actualParamStart;
				md.parameterCount = actualParamCount;
				if (md.parameterCount >= 256)
				{
					TEMP_FORMAT(errMsg, "method:%s.%s parameter count:%d is too large", _rawImage->GetStringFromRawIndex(DecodeMetadataIndex(typeDef.nameIndex)), methodName, md.parameterCount);
                    RaiseExecutionEngineException(errMsg);
				}
				for (uint32_t paramRowIndex = namedParamStart + 1; paramRowIndex <= namedParamStart + namedParamCount; paramRowIndex++)
				{
					TbParam data = _rawImage->ReadParam(paramRowIndex);
					if (data.sequence > 0)
					{
						int32_t actualParamIndex = actualParamStart + data.sequence - 1;
						ParamDetail& paramDetail = _params[actualParamIndex];
						Il2CppParameterDefinition& pd = paramDetail.paramDef;
						IL2CPP_ASSERT(paramDetail.parameterIndex == data.sequence - 1);
						pd.nameIndex = EncodeWithIndex(data.name);
						pd.token = EncodeToken(TableType::PARAM, paramRowIndex);
						(*_paramRawIndex2ActualParamIndex)[paramRowIndex - 1] = actualParamIndex;
						if (data.flags)
						{
							const Il2CppType* fieldType = il2cpp::vm::GlobalMetadata::GetIl2CppTypeFromIndex(pd.typeIndex);
							Il2CppType* newType = MetadataPool::ShallowCloneIl2CppType(fieldType);
							newType->attrs = data.flags;
							//paramDetail.type = newType;
							pd.typeIndex = AddIl2CppTypeCache(newType);
						}
					}
					else
					{
						// data.sequence == 0  is for returnType.
						// used for parent of CustomeAttributes of ReturnType
						// il2cpp not support ReturnType CustomAttributes. so we just ignore it.
#if SUPPORT_METHOD_RETURN_TYPE_CUSTOM_ATTRIBUTE
						md.returnParameterToken = EncodeToken(TableType::PARAM, paramRowIndex);
#endif
					}
				}
			}
		}
	}

	const il2cpp::utils::dynamic_array<MethodImpl> InterpreterImage::GetTypeMethodImplByTypeDefinition(const Il2CppTypeDefinition* typeDef)
	{
		uint32_t index = (uint32_t)(typeDef - &_typesDefines[0]);
		IL2CPP_ASSERT(index < (uint32_t)_typeDetails.size());
		TypeDefinitionDetail& tdd = _typeDetails[index];
		il2cpp::utils::dynamic_array<MethodImpl> methodImpls(tdd.methodImplCount);

		for (uint32_t i = 0; i < tdd.methodImplCount; i++)
		{
			uint32_t index = tdd.methodImplStart + i;
			TbMethodImpl data = _rawImage->ReadMethodImpl(index + 1);
			Il2CppTypeDefinition& typeDef = _typesDefines[data.classIdx - 1];
			Il2CppGenericContainer* gc = GetGenericContainerByTypeDefinition(&typeDef);
			MethodImpl& impl = methodImpls[i];
			ReadMethodRefInfoFromToken(gc, nullptr, DecodeMethodDefOrRefCodedIndexTableType(data.methodBody), DecodeMethodDefOrRefCodedIndexRowIndex(data.methodBody), impl.body);
			ReadMethodRefInfoFromToken(gc, nullptr, DecodeMethodDefOrRefCodedIndexTableType(data.methodDeclaration), DecodeMethodDefOrRefCodedIndexRowIndex(data.methodDeclaration), impl.declaration);
		}
		return methodImpls;
	}

	void InterpreterImage::InitMethodImpls0()
	{
		const Table& miTb = _rawImage->GetTable(TableType::METHODIMPL);
		for (uint32_t i = 0; i < miTb.rowNum; i++)
		{
			TbMethodImpl data = _rawImage->ReadMethodImpl(i + 1);
			uint32_t typeIndex = data.classIdx - 1;
			TypeDefinitionDetail& tdd = _typeDetails[typeIndex];
			Il2CppTypeDefinition& typeDef = _typesDefines[typeIndex];
			Il2CppGenericContainer* gc = GetGenericContainerByTypeDefinition(&typeDef);
			if (tdd.methodImplCount == 0)
			{
				tdd.methodImplStart = i;
			}
			++tdd.methodImplCount;
			//MethodImpl impl;
			//ReadMethodRefInfoFromToken(gc, nullptr, DecodeMethodDefOrRefCodedIndexTableType(data.methodBody), DecodeMethodDefOrRefCodedIndexRowIndex(data.methodBody), impl.body);
			//ReadMethodRefInfoFromToken(gc, nullptr, DecodeMethodDefOrRefCodedIndexTableType(data.methodDeclaration), DecodeMethodDefOrRefCodedIndexRowIndex(data.methodDeclaration), impl.declaration);
			//tdd.methodImpls.push_back(impl);
		}
	}

	void InterpreterImage::InitProperties()
	{
		const Table& propertyMapTb = _rawImage->GetTable(TableType::PROPERTYMAP);
		const Table& propertyTb = _rawImage->GetTable(TableType::PROPERTY);
		_propeties.reserve(propertyTb.rowNum);

		for (uint32_t rowIndex = 1; rowIndex <= propertyTb.rowNum; rowIndex++)
		{
			TbProperty data = _rawImage->ReadProperty(rowIndex);
			_propeties.push_back({ _rawImage->GetStringFromRawIndex(data.name), data.flags, data.type, 0, 0
				, nullptr
				, { (StringIndex)EncodeWithIndex(data.name), kMethodIndexInvalid, kMethodIndexInvalid, (uint32_t)data.flags, EncodeToken(TableType::PROPERTY, rowIndex)}
				});
		}

		Il2CppTypeDefinition* last = nullptr;
		uint32_t lastParent = 0;
		for (uint32_t rowIndex = 1; rowIndex <= propertyMapTb.rowNum; rowIndex++)
		{
			TbPropertyMap data = _rawImage->ReadPropertyMap(rowIndex);
			if (data.parent == 0 || data.parent > _typesDefines.size() || data.parent <= lastParent ||
				data.propertyList == 0 || uint64_t(data.propertyList) > uint64_t(propertyTb.rowNum) + 1)
				RaiseBadImageException("invalid interpreter property map range");
			Il2CppTypeDefinition* typeDef = &_typesDefines[data.parent - 1];
			lastParent = data.parent;
			typeDef->propertyStart = EncodeWithIndex(data.propertyList); // start from 1
			if (last != nullptr)
			{
				if (!InterpreterMetadataCounts::TryListRange(DecodeMetadataIndex(last->propertyStart),
					data.propertyList, propertyTb.rowNum, last->property_count))
					RaiseBadImageException("invalid interpreter property count");
			}
			last = typeDef;
		}
		if (last)
		{
			if (!InterpreterMetadataCounts::TryListRange(DecodeMetadataIndex(last->propertyStart),
				uint64_t(propertyTb.rowNum) + 1, propertyTb.rowNum, last->property_count))
				RaiseBadImageException("invalid final interpreter property count");
		}
#if HYBRIDCLR_UNITY_2019
		for (const Il2CppTypeDefinition& typeDef : _typesDefines)
		{
			if (typeDef.property_count == 0)
			{
				continue;
			}
			for (int32_t start = DecodeMetadataIndex(typeDef.propertyStart), i = 0; i < typeDef.property_count; i++)
			{
				_propeties[start + i - 1].declaringType = &typeDef;
			}
		}
#endif
	}

	void InterpreterImage::InitEvents()
	{
		const Table& eventMapTb = _rawImage->GetTable(TableType::EVENTMAP);
		const Table& eventTb = _rawImage->GetTable(TableType::EVENT);
		_events.reserve(eventTb.rowNum);

		for (uint32_t rowIndex = 1; rowIndex <= eventTb.rowNum; rowIndex++)
		{
			TbEvent data = _rawImage->ReadEvent(rowIndex);
			_events.push_back({ _rawImage->GetStringFromRawIndex(data.name), data.eventFlags, data.eventType, 0, 0, 0
#if HYBRIDCLR_UNITY_2019
				, nullptr
				, { (StringIndex)EncodeWithIndex(data.name), kTypeIndexInvalid, kMethodIndexInvalid, kMethodIndexInvalid, kMethodIndexInvalid, EncodeToken(TableType::EVENT, rowIndex)}
#endif
				});
		}

		Il2CppTypeDefinition* last = nullptr;
		uint32_t lastParent = 0;
		for (uint32_t rowIndex = 1; rowIndex <= eventMapTb.rowNum; rowIndex++)
		{
			TbEventMap data = _rawImage->ReadEventMap(rowIndex);
			if (data.parent == 0 || data.parent > _typesDefines.size() || data.parent <= lastParent ||
				data.eventList == 0 || uint64_t(data.eventList) > uint64_t(eventTb.rowNum) + 1)
				RaiseBadImageException("invalid interpreter event map range");
			Il2CppTypeDefinition* typeDef = &_typesDefines[data.parent - 1];
			lastParent = data.parent;
			typeDef->eventStart = EncodeWithIndex(data.eventList); // start from 1
			if (last != nullptr)
			{
				if (!InterpreterMetadataCounts::TryListRange(DecodeMetadataIndex(last->eventStart),
					data.eventList, eventTb.rowNum, last->event_count))
					RaiseBadImageException("invalid interpreter event count");
			}
			last = typeDef;
		}
		if (last)
		{
			if (!InterpreterMetadataCounts::TryListRange(DecodeMetadataIndex(last->eventStart),
				uint64_t(eventTb.rowNum) + 1, eventTb.rowNum, last->event_count))
				RaiseBadImageException("invalid final interpreter event count");
		}
#if HYBRIDCLR_UNITY_2019
		for (const Il2CppTypeDefinition& typeDef : _typesDefines)
		{
			if (typeDef.event_count == 0)
			{
				continue;
			}
			for (int32_t start = DecodeMetadataIndex(typeDef.eventStart), i = 0; i < typeDef.event_count; i++)
			{
				EventDetail& ed = _events[start + i - 1];
				ed.declaringType = &typeDef;
				ed.il2cppDefinition.typeIndex = typeDef.byvalTypeIndex;
			}
		}
#endif
	}


	void InterpreterImage::InitMethodSemantics()
	{
		const Table& msTb = _rawImage->GetTable(TableType::METHODSEMANTICS);
		for (uint32_t rowIndex = 1; rowIndex <= msTb.rowNum; rowIndex++)
		{
			TbMethodSemantics data = _rawImage->ReadMethodSemantics(rowIndex);
			uint32_t method = data.method;
			uint16_t semantics = data.semantics;
			TableType tableType = DecodeHasSemanticsCodedIndexTableType(data.association);
			uint32_t propertyOrEventIndex = DecodeHasSemanticsCodedIndexRowIndex(data.association) - 1;
			if (semantics & (uint16_t)MethodSemanticsAttributes::Getter)
			{
				IL2CPP_ASSERT(tableType == TableType::PROPERTY);
				PropertyDetail& pd = _propeties[propertyOrEventIndex];
				pd.getterMethodIndex = method;
#if HYBRIDCLR_UNITY_2019
				pd.il2cppDefinition.get = method - DecodeMetadataIndex(pd.declaringType->methodStart) - 1;
#endif
			}
			if (semantics & (uint16_t)MethodSemanticsAttributes::Setter)
			{
				IL2CPP_ASSERT(tableType == TableType::PROPERTY);
				PropertyDetail& pd = _propeties[propertyOrEventIndex];
				pd.setterMethodIndex = method;
#if HYBRIDCLR_UNITY_2019
				pd.il2cppDefinition.set = method - DecodeMetadataIndex(pd.declaringType->methodStart) - 1;
#endif
			}
			if (semantics & (uint16_t)MethodSemanticsAttributes::AddOn)
			{
				IL2CPP_ASSERT(tableType == TableType::EVENT);
				EventDetail& ed = _events[propertyOrEventIndex];
				ed.addMethodIndex = method;
#if HYBRIDCLR_UNITY_2019
				ed.il2cppDefinition.add = method - DecodeMetadataIndex(ed.declaringType->methodStart) - 1;
#endif
			}
			if (semantics & (uint16_t)MethodSemanticsAttributes::RemoveOn)
			{
				IL2CPP_ASSERT(tableType == TableType::EVENT);
				EventDetail& ed = _events[propertyOrEventIndex];
				ed.removeMethodIndex = method;
#if HYBRIDCLR_UNITY_2019
				ed.il2cppDefinition.remove = method - DecodeMetadataIndex(ed.declaringType->methodStart) - 1;
#endif
			}
			if (semantics & (uint16_t)MethodSemanticsAttributes::Fire)
			{
				IL2CPP_ASSERT(tableType == TableType::EVENT);
				EventDetail& ed = _events[propertyOrEventIndex];
				ed.fireMethodIndex = method;
#if HYBRIDCLR_UNITY_2019
				ed.il2cppDefinition.raise = method - DecodeMetadataIndex(ed.declaringType->methodStart) - 1;
#endif
			}
		}
	}

	struct EnclosingClassInfo
	{
		uint32_t enclosingTypeIndex; // rowIndex - 1
		std::vector<uint32_t> nestedTypeIndexs;
	};

	void InterpreterImage::InitNestedClass()
	{
		const Table& nestedClassTb = _rawImage->GetTable(TableType::NESTEDCLASS);
		_nestedTypeDefineIndexs.reserve(nestedClassTb.rowNum);
		std::vector<EnclosingClassInfo> enclosingTypes;

		for (uint32_t i = 0; i < nestedClassTb.rowNum; i++)
		{
			TbNestedClass data = _rawImage->ReadNestedClass(i + 1);
			Il2CppTypeDefinition& nestedType = _typesDefines[data.nestedClass - 1];
			Il2CppTypeDefinition& enclosingType = _typesDefines[data.enclosingClass - 1];
			if (enclosingType.nested_type_count == 0)
			{
				// 此行代码不能删，用于标识 enclosingTypes的index
				enclosingType.nestedTypesStart = (uint32_t)enclosingTypes.size();
				enclosingTypes.push_back({ data.enclosingClass - 1 });
			}
			++enclosingType.nested_type_count;
			enclosingTypes[enclosingType.nestedTypesStart].nestedTypeIndexs.push_back(data.nestedClass - 1);
			//_nestedTypeDefineIndexs.push_back(data.nestedClass - 1);
			nestedType.declaringTypeIndex = enclosingType.byvalTypeIndex;
		}

		for (auto& enclosingType : enclosingTypes)
		{
			Il2CppTypeDefinition& enclosingTypeDef = _typesDefines[enclosingType.enclosingTypeIndex];
			IL2CPP_ASSERT(enclosingType.nestedTypeIndexs.size() == (size_t)enclosingTypeDef.nested_type_count);
			enclosingTypeDef.nestedTypesStart = (NestedTypeIndex)_nestedTypeDefineIndexs.size();
			enclosingTypeDef.nested_type_count = (uint16_t)enclosingType.nestedTypeIndexs.size();
			_nestedTypeDefineIndexs.insert(_nestedTypeDefineIndexs.end(), enclosingType.nestedTypeIndexs.begin(), enclosingType.nestedTypeIndexs.end());
		}
	}

	void InterpreterImage::InitClassLayouts0()
	{
		const Table& classLayoutTb = _rawImage->GetTable(TableType::CLASSLAYOUT);
		for (uint32_t i = 0; i < classLayoutTb.rowNum; i++)
		{
			TbClassLayout data = _rawImage->ReadClassLayout(i + 1);
			_classLayouts[data.parent - 1] = data;
			if (data.classSize > 0)
			{
				Il2CppTypeDefinitionSizes& typeSizes = _typeDetails[data.parent - 1].typeSizes;
				typeSizes.instance_size = data.classSize + sizeof(Il2CppObject);
			}
		}
	}

	void InterpreterImage::InitClassLayouts()
	{
		ClassFieldLayoutCalculator calculator(this);
		for (Il2CppTypeDefinition& type : _typesDefines)
		{
			const Il2CppType* il2cppType = GetIl2CppTypeFromTypeDefinition(&type);
			calculator.CalcClassNotStaticFields(il2cppType);
		}

		for (TypeDefinitionDetail& type : _typeDetails)
		{
			const Il2CppTypeDefinition* typeDef = GetTypeDefinitionByTypeDetail(&type);
			const Il2CppType* il2cppType = GetIl2CppTypeFromTypeDefinition(typeDef);
			calculator.CalcClassStaticFields(il2cppType);
			ClassLayoutInfo* layout = calculator.GetClassLayoutInfo(il2cppType);

			auto& sizes = type.typeSizes;
			sizes.native_size = layout->nativeSize;
			if (typeDef->genericContainerIndex == kGenericContainerIndexInvalid)
			{
				sizes.static_fields_size = layout->staticFieldsSize;
				sizes.thread_static_fields_size = layout->threadStaticFieldsSize;
			}
			else
			{
				sizes.static_fields_size = 0;
				sizes.thread_static_fields_size = 0;
			}
			if (sizes.instance_size == 0)
			{
				sizes.instance_size = layout->instanceSize;
			}
			int32_t fieldStart = DecodeMetadataIndex(typeDef->fieldStart);
			for (int32_t i = 0, end = typeDef->field_count; i < end ; i++)
			{
				FieldDetail& fd = _fieldDetails[fieldStart + i];
				FieldLayout& fieldLayout = layout->fields[i];
				if (fd.offset == 0)
				{
					fd.offset = fieldLayout.offset;
				}
				else if (fd.offset == THREAD_LOCAL_STATIC_MASK)
				{
					fd.offset = fieldLayout.offset;
				}
				else
				{
					IL2CPP_ASSERT(fd.offset == fieldLayout.offset);
					int a = 0;
				}
			}
		}
	}

	uint32_t InterpreterImage::AddIl2CppTypeCache(const Il2CppType* type)
	{
		il2cpp::os::FastAutoLock lock(&il2cpp::vm::g_MetadataLock);
		return InterpreterTypeCacheInsertion::GetOrInsert(_types, _type2Indexs, type,
			[this](uint32_t raw) { return EncodeWithIndex(raw); });
	}

	uint32_t InterpreterImage::AddIl2CppGenericContainers(Il2CppGenericContainer& geneContainer)
	{
		uint32_t index = (uint32_t)_genericContainers.size();
		_genericContainers.push_back(geneContainer);
		return EncodeWithIndex(index);
	}

	void InterpreterImage::InitClass()
	{
		const Table& typeDefTb = _rawImage->GetTable(TableType::TYPEDEF);
		_classList.resize(typeDefTb.rowNum);
	}

	Il2CppClass* InterpreterImage::GetTypeInfoFromTypeDefinitionRawIndex(uint32_t index)
	{
		IL2CPP_ASSERT(index < _classList.size());
		Il2CppClass* klass = _classList[index];
		if (klass)
		{
			return klass;
		}
		il2cpp::os::FastAutoLock lock(&il2cpp::vm::g_MetadataLock);
		klass = _classList[index];
		if (klass)
		{
			return klass;
		}
		klass = il2cpp::vm::GlobalMetadata::FromTypeDefinition(EncodeWithIndex(index));
		IL2CPP_ASSERT(klass->interfaces_count <= klass->interface_offsets_count || _typesDefines[index].interfaceOffsetsStart == 0);
		il2cpp::os::Atomic::FullMemoryBarrier();
		_classList[index] = klass;
		return klass;
	}

	const Il2CppType* InterpreterImage::GetInterfaceFromGlobalOffset(TypeInterfaceIndex globalOffset)
	{
		il2cpp::os::FastAutoLock lock(&il2cpp::vm::g_MetadataLock);
		IL2CPP_ASSERT((uint32_t)globalOffset < (uint32_t)_interfaceDefines.size());

		TypeIndex typeIndex = _interfaceDefines[globalOffset];
		if (typeIndex == kTypeIndexInvalid)
		{
			uint32_t rowIndex = globalOffset + 1;
			TbInterfaceImpl data = _rawImage->ReadInterfaceImpl(rowIndex);
			Il2CppTypeDefinition& typeDef = _typesDefines[data.classIdx - 1];
			const Il2CppType* intType = ReadTypeFromToken(GetGenericContainerByTypeDefinition(&typeDef), nullptr,
				DecodeTypeDefOrRefOrSpecCodedIndexTableType(data.interfaceIdx), DecodeTypeDefOrRefOrSpecCodedIndexRowIndex(data.interfaceIdx));
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
			// Vtable construction for a staged type may walk an ordinary
			// interpreter parent. Resolve that parent's interface in the staged
			// caller's private world, but do not publish the resulting type into
			// the ordinary image's process-lifetime cache. Its public cache can be
			// populated later under the public resolver epoch.
			if (AssemblyShadowBridge::IsStaging() &&
				AssemblyShadowBridge::GetPrivateImage(GetIndex()) != this)
				return intType;
#endif
			_interfaceDefines[globalOffset] = typeIndex = DecodeMetadataIndex(AddIl2CppTypeCache(intType));
		}

		return _types[typeIndex];
	}

	const Il2CppType* InterpreterImage::GetInterfaceFromIndex(const Il2CppClass* klass, TypeInterfaceIndex globalOffset)
	{
		return GetInterfaceFromGlobalOffset(globalOffset);
	}

	const Il2CppType* InterpreterImage::GetInterfaceFromOffset(const Il2CppClass* klass, TypeInterfaceIndex offset)
	{
		const Il2CppTypeDefinition* typeDef = (const Il2CppTypeDefinition*)(klass->typeMetadataHandle);
		IL2CPP_ASSERT(typeDef);
		return GetInterfaceFromOffset(typeDef, offset);
	}

	const Il2CppType* InterpreterImage::GetInterfaceFromOffset(const Il2CppTypeDefinition* typeDef, TypeInterfaceIndex offset)
	{
		uint32_t globalOffset = typeDef->interfacesStart + offset;
		return GetInterfaceFromGlobalOffset(globalOffset);
	}

	Il2CppInterfaceOffsetInfo InterpreterImage::GetInterfaceOffsetInfo(const Il2CppTypeDefinition* typeDefine, TypeInterfaceOffsetIndex index)
	{
		if (index < 0 || static_cast<uint32_t>(index) >= typeDefine->interface_offsets_count)
			RaiseBadImageException("interpreter interface offset is outside the type range");
		uint32_t globalIndex = 0;
		const InterpreterImageMetadataDecoder decoder;
		const InterpreterMetadataRange::Error error = InterpreterMetadataRange::ResolveOffset(
			typeDefine->interfaceOffsetsStart, GetIndex(), static_cast<uint32_t>(index),
			static_cast<uint32_t>(_interfaceOffsets.size()), decoder, globalIndex);
		RaiseInvalidMetadataRange("invalid interpreter interface offset", error);

		InterfaceOffsetInfo& offsetPair = _interfaceOffsets[globalIndex];
		return { offsetPair.type, (int32_t)offsetPair.offset };
	}

	Il2CppClass* InterpreterImage::GetNestedTypeFromOffset(const Il2CppTypeDefinition* typeDefine, TypeNestedTypeIndex offset)
	{
		uint32_t globalIndex = typeDefine->nestedTypesStart + offset;
		IL2CPP_ASSERT(globalIndex < (uint32_t)_nestedTypeDefineIndexs.size());
		uint32_t typeDefIndex = _nestedTypeDefineIndexs[globalIndex];
		IL2CPP_ASSERT(typeDefIndex < (uint32_t)_typesDefines.size());
		return il2cpp::vm::GlobalMetadata::GetTypeInfoFromHandle((Il2CppMetadataTypeHandle)&_typesDefines[typeDefIndex]);
	}

	Il2CppClass* InterpreterImage::GetNestedTypeFromOffset(const Il2CppClass* klass, TypeNestedTypeIndex offset)
	{
		return GetNestedTypeFromOffset((Il2CppTypeDefinition*)klass->typeMetadataHandle, offset);
	}

	Il2CppTypeDefinition* InterpreterImage::GetNestedTypes(Il2CppTypeDefinition* typeDefinition, void** iter)
	{
		if (_nestedTypeDefineIndexs.empty())
		{
			return nullptr;
		}
		const TypeDefinitionIndex* nestedTypeIndices = (const TypeDefinitionIndex*)(&_nestedTypeDefineIndexs[typeDefinition->nestedTypesStart]);

		if (!*iter)
		{
			if (typeDefinition->nested_type_count == 0)
				return NULL;

			*iter = (void*)(nestedTypeIndices);
			return &_typesDefines[nestedTypeIndices[0]];
		}

		TypeDefinitionIndex* nestedTypeAddress = (TypeDefinitionIndex*)*iter;
		nestedTypeAddress++;
		ptrdiff_t index = nestedTypeAddress - nestedTypeIndices;

		if (index < typeDefinition->nested_type_count)
		{
			*iter = nestedTypeAddress;
			return &_typesDefines[*nestedTypeAddress];
		}

		return NULL;
	}

#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
    bool InterpreterImage::DeclaredReferenceHasPublicKeyToken(int32_t index) const
    {
        IL2CPP_ASSERT(index >= 0 && static_cast<size_t>(index) < _declaredAssemblyReferences.size());
        return _declaredAssemblyReferences[index].hasPublicKeyToken;
    }

    void InterpreterImage::GetDeclaredReferencedAssemblyNames(il2cpp::vm::AssemblyNameVector& target)
    {
        for (size_t index = 0; index < _declaredAssemblyReferences.size(); ++index)
        {
            const Il2CppAssemblyName& reference = _declaredAssemblyReferences[index].name;
            if (_stagedNetstandardProviders.empty() || !IsNetStandardFacadeName(reference.name))
                GetReferencedAssembly(static_cast<int32_t>(index), nullptr, 0);
            target.push_back(&reference);
        }
    }

	void InterpreterImage::BindStagedAssemblyReferences()
	{
		IL2CPP_ASSERT(AssemblyShadowBridge::IsStaging());
		for (uint32_t row = 1, count = _rawImage->GetTableRowNum(TableType::ASSEMBLYREF); row <= count; ++row)
		{
			TbAssemblyRef reference = _rawImage->ReadAssemblyRef(row);
			const char* name = _rawImage->GetStringFromRawIndex(reference.name);
			std::vector<const Il2CppAssembly*> providers;
			if (AssemblyShadowBridge::TryResolveFacadeForCurrentThread(name, providers))
			{
				if (!IsNetStandardFacadeName(name) || providers.empty())
					RaiseExecutionEngineException("Invalid private logical facade binding");
				_stagedNetstandardProviders.swap(providers);
			}
			else
                il2cpp::vm::AssemblyShadow::ResolveReferencedAssembly(GetReferenceRequester(), nullptr, name,
                    row - 1, "InterpreterImage::BindStagedAssemblyReferences");
		}
	}
#endif

	const Il2CppAssembly* InterpreterImage::GetReferencedAssembly(int32_t referencedAssemblyTableIndex, const Il2CppAssembly assembliesTable[], int assembliesCount)
	{
		auto& table = _rawImage->GetTable(TableType::ASSEMBLYREF);
		IL2CPP_ASSERT((uint32_t)referencedAssemblyTableIndex < table.rowNum);

		TbAssemblyRef assRef = _rawImage->ReadAssemblyRef(referencedAssemblyTableIndex + 1);
		const char* refAssName = _rawImage->GetStringFromRawIndex(assRef.name);
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        const Il2CppAssembly* physical = AssemblyShadowBridge::IsStaging() ? nullptr :
            il2cpp::vm::Assembly::GetLoadedAssemblyPhysical(refAssName);
        const Il2CppAssembly* il2cppAssRef = il2cpp::vm::AssemblyShadow::ResolveReferencedAssembly(
            GetReferenceRequester(), physical, refAssName, referencedAssemblyTableIndex,
            "InterpreterImage::GetReferencedAssembly");
#else
		const Il2CppAssembly* il2cppAssRef = il2cpp::vm::Assembly::GetLoadedAssembly(refAssName);
#endif
		if (!il2cppAssRef)
		{
			il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetDllNotFoundException(refAssName));
		}
		return il2cppAssRef;
	}

	void InterpreterImage::ReadFieldRefInfoFromFieldDefToken(uint32_t rowIndex, FieldRefInfo& ret)
	{
		IL2CPP_ASSERT(rowIndex > 0);
		const FieldDetail& fd = GetFieldDetailFromRawIndex(rowIndex - 1);
		ret.containerType = GetIl2CppTypeFromRawTypeDefIndex(DecodeMetadataIndex(fd.typeDefIndex));
		ret.field = &fd.fieldDef;
	}

	void InterpreterImage::GetClassAndMethodGenericContainerFromGenericContainerIndex(GenericContainerIndex idx, const Il2CppGenericContainer*& klassGc, const Il2CppGenericContainer*& methodGc)
	{
		Il2CppGenericContainer* gc = GetGenericContainerByRawIndex(DecodeMetadataIndex(idx));
		IL2CPP_ASSERT(gc);
		if (gc->is_method)
		{
			const Il2CppMethodDefinition* methodDef = GetMethodDefinitionFromRawIndex(DecodeMetadataIndex(gc->ownerIndex));
			klassGc = GetGenericContainerByTypeDefRawIndex(DecodeMetadataIndex(methodDef->declaringType));
			methodGc = GetGenericContainerByRawIndex(DecodeMetadataIndex(methodDef->genericContainerIndex));
		}
		else
		{
			klassGc = gc;
			methodGc = nullptr;
		}
	}

	void InterpreterImage::InitGenericParamConstraintDefs()
	{
		const Table& tb = _rawImage->GetTable(TableType::GENERICPARAMCONSTRAINT);
		if (_genericConstraintStarts.size() != _genericParams.size())
		{
			RaiseBadImageException("generic constraint sidecar does not match parameter table");
			return;
		}
		_genericConstraints.resize(tb.rowNum, kTypeIndexInvalid);
		int32_t lastOwner = -1;
		for (uint32_t i = 0; i < tb.rowNum; i++)
		{
			uint32_t rowIndex = i + 1;
			TbGenericParamConstraint data = _rawImage->ReadGenericParamConstraint(rowIndex);
			const InterpreterGenericConstraintMap::Error error = InterpreterGenericConstraintMap::RecordRow(
				data.owner, i, _genericParams, _genericConstraintStarts, lastOwner);
			if (error != InterpreterGenericConstraintMap::Error::None)
			{
				RaiseBadImageException("invalid interpreter generic constraint table");
				return;
			}
			// constraintsStart intentionally remains untouched: it is an int16_t
			// field shared with native AOT metadata. Interpreter lookup uses the
			// raw-start sidecar above.
			//_genericConstraints[i] == kTypeIndexInvalid;

			//Il2CppType paramCons = {};

			//const Il2CppGenericContainer* klassGc;
			//const Il2CppGenericContainer* methodGc;
			//GetClassAndMethodGenericContainerFromGenericContainerIndex(genericParam.ownerIndex, klassGc, methodGc);

			//ReadTypeFromToken(klassGc, methodGc, DecodeTypeDefOrRefOrSpecCodedIndexTableType(data.constraint), DecodeTypeDefOrRefOrSpecCodedIndexRowIndex(data.constraint), paramCons);
			//_genericConstraints[i] = DecodeMetadataIndex(AddIl2CppTypeCache(paramCons));
		}
	}

	void InterpreterImage::InitGenericParamDefs0()
	{
		const Table& tb = _rawImage->GetTable(TableType::GENERICPARAM);
		_genericParams.resize(tb.rowNum);
		InterpreterGenericConstraintMap::Initialize(_genericConstraintStarts, tb.rowNum);
	}

	void InterpreterImage::InitGenericParamDefs()
	{
		const Table& tb = _rawImage->GetTable(TableType::GENERICPARAM);
		for (uint32_t i = 0; i < tb.rowNum; i++)
		{
			uint32_t rowIndex = i + 1;
			TbGenericParam data = _rawImage->ReadGenericParam(rowIndex);
			Il2CppGenericParameter& paramDef = _genericParams[i];
			paramDef.num = data.number;
			paramDef.flags = data.flags;
			paramDef.nameIndex = EncodeWithIndex(data.name);
			// constraintsStart 和 constrantsCount init at InitGenericParamConstrains() latter

			TableType ownerType = DecodeTypeOrMethodDefCodedIndexTableType(data.owner);
			uint32_t ownerIndex = DecodeTypeOrMethodDefCodedIndexRowIndex(data.owner);
			IL2CPP_ASSERT(ownerIndex > 0);
			Il2CppGenericContainer* geneContainer;
			int32_t interIndex = ownerIndex - 1;
			if (ownerType == TableType::TYPEDEF)
			{
				Il2CppTypeDefinition& typeDef = _typesDefines[interIndex];
				if (typeDef.genericContainerIndex == kGenericContainerIndexInvalid)
				{
					Il2CppGenericContainer c = {};
					c.ownerIndex = EncodeWithIndex(interIndex);
					c.is_method = false;
					typeDef.genericContainerIndex = AddIl2CppGenericContainers(c);
				}
				geneContainer = &_genericContainers[DecodeMetadataIndex(typeDef.genericContainerIndex)];
				paramDef.ownerIndex = typeDef.genericContainerIndex;
			}
			else
			{
				Il2CppMethodDefinition& methodDef = _methodDefines[interIndex];
				if (methodDef.genericContainerIndex == kGenericContainerIndexInvalid)
				{
					Il2CppGenericContainer c = {};
					c.ownerIndex = EncodeWithIndex(interIndex);
					c.is_method = true;
					methodDef.genericContainerIndex = AddIl2CppGenericContainers(c);
				}
				geneContainer = &_genericContainers[DecodeMetadataIndex(methodDef.genericContainerIndex)];
				paramDef.ownerIndex = methodDef.genericContainerIndex;
			}
			if (geneContainer->type_argc == 0)
			{
				geneContainer->genericParameterStart = EncodeWithIndex(i);
			}
			++geneContainer->type_argc;
		}
	}


	void InterpreterImage::InitInterfaces()
	{
		const Table& table = _rawImage->GetTable(TableType::INTERFACEIMPL);

		// interface中只包含直接继承的interface,不包括来自父类的
		// 此interface只在CastClass及Type.GetInterfaces()反射函数中
		// 发挥作用，不在callvir中发挥作用。
		// interfaceOffsets中包含了水平展开的所有interface(包括父类的)
		_interfaceDefines.resize(table.rowNum, kTypeIndexInvalid);
		uint32_t lastClassIdx = 0;
		for (uint32_t i = 0; i < table.rowNum; i++)
		{
			uint32_t rowIndex = i + 1;
			TbInterfaceImpl data = _rawImage->ReadInterfaceImpl(rowIndex);

			if (data.classIdx == 0 || data.classIdx > _typesDefines.size())
				RaiseBadImageException("invalid interpreter interface owner");
			Il2CppTypeDefinition& typeDef = _typesDefines[data.classIdx - 1];
			if (!InterpreterMetadataCounts::CanAppend(typeDef.interfaces_count, 1))
				RaiseBadImageException("interpreter interface count exceeds native limit");
			//Il2CppType intType = {};
			//ReadTypeFromToken(GetGenericContainerByTypeDefinition(&typeDef), nullptr,
			//	DecodeTypeDefOrRefOrSpecCodedIndexTableType(data.interfaceIdx), DecodeTypeDefOrRefOrSpecCodedIndexRowIndex(data.interfaceIdx), intType);
			//_interfaceDefines[i] = DecodeMetadataIndex(AddIl2CppTypeCache(intType));
			if (typeDef.interfaces_count == 0)
			{
				typeDef.interfacesStart = (InterfacesIndex)i;
			}
			else
			{
				// 必须连续
				if (data.classIdx != lastClassIdx)
					RaiseBadImageException("noncontiguous interpreter interface range");
			}
			++typeDef.interfaces_count;
			lastClassIdx = data.classIdx;
		}
	}

	void InterpreterImage::ComputeVTable(TypeDefinitionDetail* tdd)
	{
		Il2CppTypeDefinition& typeDef = *GetTypeDefinitionByTypeDetail(tdd);
		if (IsInterface(typeDef.flags) || typeDef.interfaceOffsetsStart != 0)
		{
			return;
		}

		if (typeDef.parentIndex != kInvalidIndex)
		{
			const Il2CppType* parentType = il2cpp::vm::GlobalMetadata::GetIl2CppTypeFromIndex(typeDef.parentIndex);
			const Il2CppTypeDefinition* parentTypeDef = GetUnderlyingTypeDefinition(parentType);
			if (IsInterpreterType(parentTypeDef) && parentTypeDef->interfaceOffsetsStart == 0)
			{
				IL2CPP_ASSERT(DecodeImageIndex(parentTypeDef->byvalTypeIndex) == this->GetIndex());
				int32_t typeDefIndex = GetTypeRawIndexByEncodedIl2CppTypeIndex(parentTypeDef->byvalTypeIndex);
				ComputeVTable(&_typeDetails[typeDefIndex]);
			}
		}

		const Il2CppType* type = GetIl2CppTypeFromRawIndex(DecodeMetadataIndex(typeDef.byvalTypeIndex));
		VTableSetUp* typeTree = VTableSetUp::BuildByType(_cacheTrees, type);

		uint32_t offsetsStart = (uint32_t)_interfaceOffsets.size();

		auto& vms = typeTree->GetVirtualMethodImpls();
		auto& interfaceOffsetInfos = typeTree->GetInterfaceOffsetInfos();
		if (!InterpreterMetadataCounts::CanAppend(vms.size(), 0) ||
			!InterpreterMetadataCounts::CanAppend(interfaceOffsetInfos.size(), 0))
			RaiseBadImageException("interpreter vtable count exceeds native limit");
		if (vms.empty())
		{
			tdd->vtable = nullptr;
			tdd->vtableCount = 0;
		}
		else
		{
			tdd->vtable = (VirtualMethodImpl*)HYBRIDCLR_METADATA_CALLOC(vms.size(), sizeof(VirtualMethodImpl));
			tdd->vtableCount = (uint32_t)vms.size();
			std::memcpy(tdd->vtable, &vms[0], vms.size() * sizeof(VirtualMethodImpl));
		}

		for (auto ioi : interfaceOffsetInfos)
		{
			_interfaceOffsets.push_back({ ioi.type, ioi.offset });
		}

		typeDef.vtableStart = EncodeWithIndex(0);
		typeDef.vtable_count = (uint16_t)vms.size();
		typeDef.interfaceOffsetsStart = EncodeWithIndex(offsetsStart);
		typeDef.interface_offsets_count = (uint16_t)interfaceOffsetInfos.size();

		Il2CppClass* klass = _classList[GetTypeRawIndex(&typeDef)];
		IL2CPP_ASSERT(!klass);
	}

	void InterpreterImage::InitVTables()
	{
		const Table& typeDefTb = _rawImage->GetTable(TableType::TYPEDEF);

		for (TypeDefinitionDetail& td : _typeDetails)
		{
			ComputeVTable(&td);
		}

		for (auto& e : _cacheTrees)
		{
			e.second->~VTableSetUp();
			HYBRIDCLR_FREE(e.second);
		}
		Il2CppType2TypeDeclaringTreeMap temp;
		_cacheTrees.swap(temp);
	}

	// index => MethodDefinition -> DeclaringClass -> index - klass->methodStart -> MethodInfo*
	const MethodInfo* InterpreterImage::GetMethodInfoFromMethodDefinitionRawIndex(uint32_t index)
	{
		IL2CPP_ASSERT((size_t)index <= _methodDefines.size());
		const Il2CppMethodDefinition* methodDefinition = GetMethodDefinitionFromRawIndex(index);
		const Il2CppTypeDefinition* typeDefinition = (const Il2CppTypeDefinition*)il2cpp::vm::GlobalMetadata::GetTypeHandleFromIndex(methodDefinition->declaringType);
		int32_t indexInClass = index - DecodeMetadataIndex(typeDefinition->methodStart);
		IL2CPP_ASSERT(indexInClass >= 0 && indexInClass < typeDefinition->method_count);
		Il2CppClass* klass = il2cpp::vm::GlobalMetadata::GetTypeInfoFromHandle((Il2CppMetadataTypeHandle)typeDefinition);
		il2cpp::vm::Class::SetupMethods(klass);
		return klass->methods[indexInClass];
	}

	const MethodInfo* InterpreterImage::GetMethodInfoFromMethodDefinition(const Il2CppMethodDefinition* methodDef)
	{
		uint32_t rawIndex = (uint32_t)(methodDef - &_methodDefines[0]);
		IL2CPP_ASSERT(rawIndex < (uint32_t)_methodDefines.size());
		return GetMethodInfoFromMethodDefinitionRawIndex(rawIndex);
	}

	// typeDef vTableSlot -> type virtual method index -> MethodDefinition*
	const Il2CppMethodDefinition* InterpreterImage::GetMethodDefinitionFromVTableSlot(const Il2CppTypeDefinition* typeDef, int32_t vTableSlot)
	{
		uint32_t typeDefIndex = GetTypeRawIndex(typeDef);
		IL2CPP_ASSERT(typeDefIndex < (uint32_t)_typeDetails.size());
		TypeDefinitionDetail& td = _typeDetails[typeDefIndex];

		IL2CPP_ASSERT(vTableSlot >= 0 && vTableSlot < (int32_t)td.vtableCount);
		VirtualMethodImpl& vmi = td.vtable[vTableSlot];
		return vmi.method;
	}

	const MethodInfo* InterpreterImage::GetMethodInfoFromVTableSlot(const Il2CppClass* klass, int32_t vTableSlot)
	{
		IL2CPP_ASSERT(!klass->generic_class);
		const Il2CppTypeDefinition* typeDef = (Il2CppTypeDefinition*)klass->typeMetadataHandle;
		//const Il2CppMethodDefinition* methodDef = GetMethodDefinitionFromVTableSlot((Il2CppTypeDefinition*)klass->typeMetadataHandle, vTableSlot);
		// FIX ME. why return null?
		//IL2CPP_ASSERT(methodDef);

		uint32_t typeDefIndex = GetTypeRawIndex(typeDef);
		IL2CPP_ASSERT(typeDefIndex < (uint32_t)_typeDetails.size());
		TypeDefinitionDetail& td = _typeDetails[typeDefIndex];

		IL2CPP_ASSERT(vTableSlot >= 0 && vTableSlot < (int32_t)td.vtableCount);
		VirtualMethodImpl& vmi = td.vtable[vTableSlot];
		if (vmi.method)
		{
			if (vmi.method->declaringType == EncodeWithIndex(typeDefIndex))
			{
				return il2cpp::vm::GlobalMetadata::GetMethodInfoFromMethodHandle((Il2CppMetadataMethodDefinitionHandle)vmi.method);
			}
			else
			{
				Il2CppClass* implClass = il2cpp::vm::Class::FromIl2CppType(vmi.type);
				IL2CPP_ASSERT(implClass != klass);
				il2cpp::vm::Class::SetupMethods(implClass);
				for (uint32_t i = 0; i < implClass->method_count; i++)
				{
					const MethodInfo* implMethod = implClass->methods[i];
					if (implMethod->token == vmi.method->token)
					{
						return implMethod;
					}
				}
				RaiseExecutionEngineException("not find vtable method");
			}
		}
		return nullptr;
	}

	Il2CppMethodPointer InterpreterImage::GetAdjustorThunk(uint32_t token)
	{
		uint32_t methodIndex = DecodeTokenRowIndex(token) - 1;
		IL2CPP_ASSERT(methodIndex < (uint32_t)_methodDefines.size());
		const Il2CppMethodDefinition* methodDef = &_methodDefines[methodIndex];
		return IsInstanceMethod(methodDef) ? hybridclr::interpreter::InterpreterModule::GetAdjustThunkMethodPointer(methodDef) : nullptr;
	}

	Il2CppMethodPointer InterpreterImage::GetMethodPointer(uint32_t token)
	{
		uint32_t methodIndex = DecodeTokenRowIndex(token) - 1;
		IL2CPP_ASSERT(methodIndex < (uint32_t)_methodDefines.size());
		const Il2CppMethodDefinition* methodDef = &_methodDefines[methodIndex];
		return hybridclr::interpreter::InterpreterModule::GetMethodPointer(methodDef);
	}

	InvokerMethod InterpreterImage::GetMethodInvoker(uint32_t token)
	{
		uint32_t methodIndex = DecodeTokenRowIndex(token) - 1;
		IL2CPP_ASSERT(methodIndex < (uint32_t)_methodDefines.size());
		const Il2CppMethodDefinition* methodDef = &_methodDefines[methodIndex];
		return hybridclr::interpreter::InterpreterModule::GetMethodInvoker(methodDef);
	}


	Il2CppString* InterpreterImage::ReadSerString(BlobReader& reader)
	{
		byte b = reader.PeekByte();
		if (b == 0xFF)
		{
			reader.SkipByte();
			return nullptr;
		}
		else if (b == 0)
		{
			reader.SkipByte();
			return il2cpp::vm::String::Empty();
		}
		else
		{
			uint32_t len = reader.ReadCompressedUint32();
#if !HYBRIDCLR_UNITY_2021_OR_NEW
			return il2cpp::vm::String::NewLen((char*)reader.GetAndSkipCurBytes(len), len);
#else
			char* chars = (char*)reader.GetDataOfReadPosition();
			reader.SkipBytes(len);
			return il2cpp::vm::String::NewLen(chars, len);
#endif
		}
	}

#if HYBRIDCLR_UNITY_2021_OR_NEW
	bool InterpreterImage::ReadUTF8SerString(BlobReader& reader, std::string& s)
	{
		byte b = reader.PeekByte();
		if (b == 0xFF)
		{
			reader.SkipByte();
			return false;
		}
		else if (b == 0)
		{
			reader.SkipByte();
			s.clear();
			return true;
		}
		else
		{
			uint32_t len = reader.ReadCompressedUint32();
			char* chars = (char*)reader.GetDataOfReadPosition();
			reader.SkipBytes(len);
			s.assign(chars, len);
			return true;
		}
	}
#endif

	Il2CppReflectionType* InterpreterImage::ReadSystemType(BlobReader& reader)
	{
		Il2CppString* fullName = ReadSerString(reader);
		if (!fullName)
		{
			return nullptr;
		}
		Il2CppReflectionType* type = GetReflectionTypeFromName(fullName);
		if (!type)
		{
			std::string stdTypeName = il2cpp::utils::StringUtils::Utf16ToUtf8(fullName->chars);
			TEMP_FORMAT(errMsg, "CustomAttribute fixed arg type:System.Type fullName:'%s' not find", stdTypeName.c_str());
			il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetTypeLoadException(errMsg));
		}
		return type;
	}


	Il2CppObject* InterpreterImage::ReadBoxedValue(BlobReader& reader)
	{
		uint64_t obj = 0;
		Il2CppType kind = {};
		ReadCustomAttributeFieldOrPropType(reader, kind);
		ReadFixedArg(reader, &kind, &obj);
		Il2CppClass* valueType = il2cpp::vm::Class::FromIl2CppType(&kind);
		return il2cpp::vm::Object::Box(valueType, &obj);
	}

	void InterpreterImage::ReadFixedArg(BlobReader& reader, const Il2CppType* argType, void* data)
	{
		switch (argType->type)
		{
		case IL2CPP_TYPE_BOOLEAN:
		{
			*(byte*)data = reader.ReadByte();
			break;
		}
		case IL2CPP_TYPE_CHAR:
		{
			*(uint16_t*)data = reader.Read16();
			break;
		}
		case IL2CPP_TYPE_I1:
		case IL2CPP_TYPE_U1:
		{
			*(byte*)data = reader.ReadByte();
			break;
		}
		case IL2CPP_TYPE_I2:
		case IL2CPP_TYPE_U2:
		{
			*(uint16_t*)data = reader.Read16();
			break;
		}
		case IL2CPP_TYPE_I4:
		case IL2CPP_TYPE_U4:
		{
			*(uint32_t*)data = reader.Read32();
			break;
		}
		case IL2CPP_TYPE_I8:
		case IL2CPP_TYPE_U8:
		{
			*(uint64_t*)data = reader.Read64();
			break;
		}
		case IL2CPP_TYPE_R4:
		{
			*(float*)data = reader.ReadFloat();
			break;
		}
		case IL2CPP_TYPE_R8:
		{
			*(double*)data = reader.ReadDouble();
			break;
		}
		case IL2CPP_TYPE_SZARRAY:
		{
			uint32_t numElem = reader.Read32();
			if (numElem != (uint32_t)-1)
			{
				Il2CppClass* arrKlass = il2cpp::vm::Class::FromIl2CppType(argType);
				Il2CppArray* arr = il2cpp::vm::Array::New(il2cpp::vm::Class::GetElementClass(arrKlass), numElem);
				for (uint16_t i = 0; i < numElem; i++)
				{
					ReadFixedArg(reader, argType->data.type, GET_ARRAY_ELEMENT_ADDRESS(arr, i, arr->klass->element_size));
				}
				*(void**)data = arr;
			}
			else
			{
				*(void**)data = nullptr;
			}
			HYBRIDCLR_SET_WRITE_BARRIER((void**)data);
			break;
		}
		case IL2CPP_TYPE_STRING:
		{
			*(Il2CppString**)data = ReadSerString(reader);
			HYBRIDCLR_SET_WRITE_BARRIER((void**)data);
			break;
		}
		case IL2CPP_TYPE_OBJECT:
		{
			*(Il2CppObject**)data = ReadBoxedValue(reader);
			HYBRIDCLR_SET_WRITE_BARRIER((void**)data);
			break;
		}
		case IL2CPP_TYPE_CLASS:
		{
			Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(argType);
			if (!klass)
			{
				RaiseExecutionEngineException("type not find");
			}
			if (klass == il2cpp_defaults.object_class)
			{
				*(Il2CppObject**)data = ReadBoxedValue(reader);
			}
			else if (klass == il2cpp_defaults.systemtype_class)
			{
				*(Il2CppReflectionType**)data = ReadSystemType(reader);
			}
			else
			{
				TEMP_FORMAT(errMsg, "fixed arg type:%s.%s not support", klass->namespaze, klass->name);
				RaiseNotSupportedException(errMsg);
			}
			HYBRIDCLR_SET_WRITE_BARRIER((void**)data);
			break;
		}
		case IL2CPP_TYPE_VALUETYPE:
		{
			Il2CppClass* valueType = il2cpp::vm::Class::FromIl2CppType(argType);
			IL2CPP_ASSERT(valueType->enumtype);
			ReadFixedArg(reader, &valueType->element_class->byval_arg, data);
			break;
		}
		case IL2CPP_TYPE_SYSTEM_TYPE:
		{
			*(Il2CppReflectionType**)data = ReadSystemType(reader);
			HYBRIDCLR_SET_WRITE_BARRIER((void**)data);
			break;
		}
		case IL2CPP_TYPE_BOXED_OBJECT:
		{
			uint8_t fieldOrPropType = reader.ReadByte();
			IL2CPP_ASSERT(fieldOrPropType == 0x51);
			*(Il2CppObject**)data = ReadBoxedValue(reader);
			HYBRIDCLR_SET_WRITE_BARRIER((void**)data);
			break;
		}
		case IL2CPP_TYPE_ENUM:
		{
			Il2CppClass* valueType = il2cpp::vm::Class::FromIl2CppType(argType);
			IL2CPP_ASSERT(valueType->enumtype);
			ReadFixedArg(reader, &valueType->element_class->byval_arg, data);
			break;
		}
		default:
		{
			RaiseExecutionEngineException("not support fixed argument type");
		}
		}
	}

	void InterpreterImage::ReadCustomAttributeFieldOrPropType(BlobReader& reader, Il2CppType& type)
	{
		type.type = (Il2CppTypeEnum)reader.ReadByte();

		switch (type.type)
		{
		case IL2CPP_TYPE_BOOLEAN:
		case IL2CPP_TYPE_CHAR:
		case IL2CPP_TYPE_I1:
		case IL2CPP_TYPE_U1:
		case IL2CPP_TYPE_I2:
		case IL2CPP_TYPE_U2:
		case IL2CPP_TYPE_I4:
		case IL2CPP_TYPE_U4:
		case IL2CPP_TYPE_I8:
		case IL2CPP_TYPE_U8:
		case IL2CPP_TYPE_R4:
		case IL2CPP_TYPE_R8:
		case IL2CPP_TYPE_STRING:
		{
			break;
		}
			case IL2CPP_TYPE_SZARRAY:
			{
				Il2CppType eleType = {};
				ReadCustomAttributeFieldOrPropType(reader, eleType);
				// Recursive resolution is callback-capable.  Pool only the completed
				// element value under the metadata lock, after that resolution returns.
				{
					il2cpp::os::FastAutoLock metaLock(&il2cpp::vm::g_MetadataLock);
					type.data.type = MetadataPool::GetPooledIl2CppType(eleType);
				}
				break;
		}
		case IL2CPP_TYPE_ENUM:
		{
			Il2CppString* enumTypeName = ReadSerString(reader);

			Il2CppReflectionType* enumType = GetReflectionTypeFromName(enumTypeName);
			if (!enumType)
			{
				std::string stdStrName = il2cpp::utils::StringUtils::Utf16ToUtf8(enumTypeName->chars);
				TEMP_FORMAT(errMsg, "ReadCustomAttributeFieldOrPropType enum:'%s' not exists", stdStrName.c_str());
				RaiseExecutionEngineException(errMsg);
			}
			type = *enumType->type;
			break;
		}
		case IL2CPP_TYPE_SYSTEM_TYPE:
		{
			type = il2cpp_defaults.systemtype_class->byval_arg;
			break;
		}
		case IL2CPP_TYPE_BOXED_OBJECT:
		{
			type = il2cpp_defaults.object_class->byval_arg;
			break;
		}
		default:
		{
			TEMP_FORMAT(errMsg, "ReadCustomAttributeFieldOrPropType. image:%s unknown type:%d", GetIl2CppImage()->name, (int)type.type);
			RaiseBadImageException(errMsg);
		}
		}
	}

	void InterpreterImage::ReadMethodDefSig(BlobReader& reader, const Il2CppGenericContainer* klassGenericContainer, const Il2CppGenericContainer* methodGenericContainer, Il2CppMethodDefinition& methodDef, std::vector<ParamDetail>& paramArr)
	{
		uint8_t rawSigFlags = reader.ReadByte();

		if (rawSigFlags & (uint8_t)MethodSigFlags::GENERIC)
		{
			//IL2CPP_ASSERT(false);
			uint32_t genParamCount = reader.ReadCompressedUint32();
			Il2CppGenericContainer* gc = GetGenericContainerByRawIndex(DecodeMetadataIndex(methodDef.genericContainerIndex));
			IL2CPP_ASSERT(gc->type_argc == genParamCount);
		}
		uint32_t paramCount = reader.ReadCompressedUint32();
		//IL2CPP_ASSERT(paramCount >= methodDef.parameterCount);

		const Il2CppType* returnType = ReadType(reader, klassGenericContainer, methodGenericContainer);
		methodDef.returnType = AddIl2CppTypeCache(returnType);

		int readParamNum = 0;
		for (; reader.NonEmpty(); )
		{
			ParamDetail curParam = {};
			const Il2CppType* type = ReadType(reader, klassGenericContainer, methodGenericContainer);
			curParam.parameterIndex = readParamNum++;
			curParam.paramDef.typeIndex = AddIl2CppTypeCache(type);
			paramArr.push_back(curParam);
		}
		IL2CPP_ASSERT(readParamNum == (int)paramCount);
	}

	const Il2CppType* InterpreterImage::GetModuleIl2CppType(uint32_t moduleRowIndex, uint32_t typeNamespace, uint32_t typeName, bool raiseExceptionIfNotFound)
	{
		IL2CPP_ASSERT(moduleRowIndex == 1);
		uint32_t encodedNamespaceIndex = EncodeWithIndex(typeNamespace);
		uint32_t encodedNameIndex = EncodeWithIndex(typeName);
		for (TypeDefinitionDetail& type : _typeDetails)
		{
			Il2CppTypeDefinition* typeDef = GetTypeDefinitionByTypeDetail(&type);
			if (typeDef->namespaceIndex == encodedNamespaceIndex && typeDef->nameIndex == encodedNameIndex)
			{
				return GetIl2CppTypeFromTypeDefinition(typeDef);
			}
		}
		if (!raiseExceptionIfNotFound)
		{
			return nullptr;
		}
		const char* typeNameStr = _rawImage->GetStringFromRawIndex(typeName);
		const char* typeNamespaceStr = _rawImage->GetStringFromRawIndex(typeNamespace);
		il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetTypeLoadException(
			CStringToStringView(typeNamespaceStr),
			CStringToStringView(typeNameStr),
			CStringToStringView(_il2cppImage->nameNoExt)));
		return nullptr;
	}
}
}
