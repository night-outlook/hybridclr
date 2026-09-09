#pragma once

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

#include "CustomAttributeDataWriter.h"

namespace hybridclr
{
namespace metadata
{
	// Attribute conversion can invoke reflection and re-enter metadata loading.
	// Keep all type-index work in this invocation-local batch until the complete
	// attribute range has successfully converted.
	class CustomAttributeTypeIndexBatch
	{
	public:
		struct Fixup
		{
			const CustomAttributeDataWriter* writer;
			uint32_t outputOffset;
			uint32_t typeIndex;
		};

		struct TypeKey
		{
			uintptr_t data;
			uint32_t type;
			uint32_t attrs;
			uint32_t byref;
			uint32_t pinned;
#if HYBRIDCLR_UNITY_2021_OR_NEW
			uint32_t valuetype;
#endif

			bool operator==(const TypeKey& other) const
			{
				return data == other.data && type == other.type && attrs == other.attrs &&
					byref == other.byref && pinned == other.pinned
#if HYBRIDCLR_UNITY_2021_OR_NEW
					&& valuetype == other.valuetype
#endif
					;
			}
		};

		struct TypeKeyHash
		{
			size_t operator()(const TypeKey& key) const
			{
				size_t hash = std::hash<uintptr_t>()(key.data);
				hash ^= std::hash<uint32_t>()(key.type) + (hash << 6) + (hash >> 2);
				hash ^= std::hash<uint32_t>()(key.attrs) + (hash << 6) + (hash >> 2);
				hash ^= std::hash<uint32_t>()(key.byref) + (hash << 6) + (hash >> 2);
				hash ^= std::hash<uint32_t>()(key.pinned) + (hash << 6) + (hash >> 2);
#if HYBRIDCLR_UNITY_2021_OR_NEW
				hash ^= std::hash<uint32_t>()(key.valuetype) + (hash << 6) + (hash >> 2);
#endif
				return hash;
			}
		};

		uint32_t AddType(const Il2CppType* type)
		{
			if (type == nullptr)
			{
				RaiseExecutionEngineException("null custom attribute type");
			}
			TypeKey key = MakeKey(type);
			auto existing = _localTypeIndexes.find(key);
			if (existing != _localTypeIndexes.end())
				return existing->second;
			if (_types.size() >= UINT32_MAX)
			{
				RaiseExecutionEngineException("custom attribute type batch overflow");
			}
			const size_t nextSize = _types.size() + 1;
			if (_types.capacity() < nextSize)
			{
				const size_t limit = std::min(_types.max_size(), static_cast<size_t>(UINT32_MAX));
				if (nextSize > limit)
					RaiseExecutionEngineException("custom attribute type batch exceeds vector limit");
				const size_t capacity = _types.capacity();
				const size_t increment = capacity / 2 + 1;
				_types.reserve(increment > limit - capacity ? limit : capacity + increment);
			}
			Il2CppType* clonedType = CloneType(type);
			uint32_t typeIndex = (uint32_t)_types.size();
			_localTypeIndexes.emplace(key, typeIndex);
			// Both vectors and the local map have capacity before ownership or
			// logical state changes, so a failed clone leaves no half-published key.
			_types.push_back(clonedType);
			return typeIndex;
		}

		void WriteTypeIndex(CustomAttributeDataWriter& writer, const Il2CppType* type)
		{
			uint32_t typeIndex = AddType(type);
			if (typeIndex == UINT32_MAX)
				return;
			uint32_t outputOffset = writer.WriteCompressedInt32Placeholder();
			_fixups.push_back({ &writer, outputOffset, typeIndex });
		}

		void AppendTo(CustomAttributeDataWriter& destination, const CustomAttributeDataWriter& source)
		{
			uint32_t destinationOffset = destination.Size();
			destination.Write(source);
			for (Fixup& fixup : _fixups)
			{
				if (fixup.writer == &source)
				{
					if (destinationOffset > UINT32_MAX - fixup.outputOffset)
					{
						RaiseExecutionEngineException("custom attribute fixup relocation overflow");
					}
					fixup.outputOffset += destinationOffset;
					fixup.writer = &destination;
				}
			}
		}

		const std::vector<const Il2CppType*>& Types() const { return _types; }
		const std::vector<Fixup>& Fixups() const { return _fixups; }

		// Ownership is transferred to the image immediately before publication.
		// The metadata type graph is intentionally process-lived like other
		// entries in the image's type cache.
		void ReleaseOwnership()
		{
			for (auto& type : _ownedTypes)
				type.release();
			for (auto& array : _ownedArrays)
				array.release();
			for (auto& values : _ownedArrayValues)
				values.release();
		}

		void DiscardOwnership()
		{
			_fixups.clear();
			_types.clear();
			_localTypeIndexes.clear();
			_ownedTypes.clear();
			_ownedArrays.clear();
			_ownedArrayValues.clear();
		}

	private:
		static TypeKey MakeKey(const Il2CppType* type)
		{
			TypeKey key = {
				reinterpret_cast<uintptr_t>(type->data.dummy),
				(uint32_t)type->type,
				(uint32_t)type->attrs,
				(uint32_t)type->byref,
				(uint32_t)type->pinned
#if HYBRIDCLR_UNITY_2021_OR_NEW
				, (uint32_t)type->valuetype
#endif
			};
			return key;
		}

		Il2CppType* CloneType(const Il2CppType* source)
		{
			std::unique_ptr<Il2CppType> ownedCopy(new Il2CppType(*source));
			Il2CppType* copy = ownedCopy.get();
			_ownedTypes.emplace_back(std::move(ownedCopy));
			switch (source->type)
			{
			case IL2CPP_TYPE_PTR:
			case IL2CPP_TYPE_SZARRAY:
				if (source->data.type)
					copy->data.type = CloneType(source->data.type);
				break;
			case IL2CPP_TYPE_ARRAY:
				if (source->data.array)
				{
					const Il2CppArrayType* sourceArray = source->data.array;
					std::unique_ptr<Il2CppArrayType> ownedArray(new Il2CppArrayType(*sourceArray));
					Il2CppArrayType* array = ownedArray.get();
					_ownedArrays.emplace_back(std::move(ownedArray));
					array->etype = sourceArray->etype ? CloneType(sourceArray->etype) : nullptr;
					if (sourceArray->numsizes && sourceArray->sizes)
					{
						std::unique_ptr<int[]> ownedValues(new int[sourceArray->numsizes]);
						int* values = ownedValues.get();
						_ownedArrayValues.emplace_back(std::move(ownedValues));
						std::memcpy(values, sourceArray->sizes, sizeof(int) * sourceArray->numsizes);
						array->sizes = values;
					}
					if (sourceArray->numlobounds && sourceArray->lobounds)
					{
						std::unique_ptr<int[]> ownedValues(new int[sourceArray->numlobounds]);
						int* values = ownedValues.get();
						_ownedArrayValues.emplace_back(std::move(ownedValues));
						std::memcpy(values, sourceArray->lobounds, sizeof(int) * sourceArray->numlobounds);
						array->lobounds = values;
					}
					copy->data.array = array;
				}
				break;
			default:
				break;
			}
			return copy;
		}

		std::vector<const Il2CppType*> _types;
		std::vector<Fixup> _fixups;
		std::unordered_map<TypeKey, uint32_t, TypeKeyHash> _localTypeIndexes;
		std::vector<std::unique_ptr<Il2CppType>> _ownedTypes;
		std::vector<std::unique_ptr<Il2CppArrayType>> _ownedArrays;
		std::vector<std::unique_ptr<int[]>> _ownedArrayValues;
	};
}
}
