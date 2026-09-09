#pragma once

#include <limits>

#include "BlobReader.h"

#include "../CommonDef.h"

#include "utils/MemoryRead.h"

namespace hybridclr
{
namespace metadata
{
	class CustomAttributeDataWriter
	{
	private:
		uint8_t* _data;
		uint32_t _capacity;
		uint32_t _size;

	public:
		CustomAttributeDataWriter(uint32_t capacity) : _capacity(Round2Exp(capacity)), _size(0)
		{
			_data = (uint8_t*)HYBRIDCLR_MALLOC_ZERO(_capacity);
			if (_data == nullptr)
			{
				il2cpp::vm::Exception::RaiseOutOfMemoryException();
			}
		}

		~CustomAttributeDataWriter()
		{
			HYBRIDCLR_FREE(_data);
			_data = nullptr;
		}

		uint32_t Size() const { return _size; }

		bool Empty() const { return _size == 0; }

		const uint8_t* Data() const { return _data; }

		const uint8_t* DataAt(uint32_t offset) { return _data + offset; }

		void Reset()
		{
			_size = 0;
		}

		void WriteAttributeCount(uint32_t count)
		{
			WriteCompressedUint32(count);
		}

		void Skip(int32_t skipBytes)
		{
			if (skipBytes < 0)
				RaiseExecutionEngineException("negative custom attribute output skip");
			SureRemainSize(static_cast<uint32_t>(skipBytes));
			_size += skipBytes;
		}

		void WriteMethodIndex(int32_t offset, int32_t methodIndex)
		{
			if (offset < 0 || static_cast<uint32_t>(offset) > _size ||
				sizeof(methodIndex) > _size - static_cast<uint32_t>(offset))
				RaiseExecutionEngineException("custom attribute method index offset out of range");
			std::memcpy(_data + offset, &methodIndex, sizeof(methodIndex));
		}

		void WriteByte(uint8_t n)
		{
			SureRemainSize(1);
			_data[_size++] = n;
		}

		void WriteCompressedUint32(uint32_t n)
		{
			SureRemainSize(5);
			uint8_t* buf = _data + _size;
			if (n < 0x80)
			{
				buf[0] = (uint8_t)n;
				++_size;
			}
			else if (n < 0x4000)
			{
				uint32_t v = n | 0x8000;
				buf[0] = uint8_t(v >> 8);
				buf[1] = uint8_t(v);
				_size += 2;
			}
			else if (n < 0x20000000)
			{
				uint32_t v = n | 0xC0000000;
				buf[0] = uint8_t(v >> 24);
				buf[1] = uint8_t(v >> 16);
				buf[2] = uint8_t(v >> 8);
				buf[3] = uint8_t(v);
				_size += 4;
			}
			else if (n < UINT32_MAX - 1)
			{
				buf[0] = 0xF0;
				buf[1] = uint8_t(n);
				buf[2] = uint8_t(n >> 8);
				buf[3] = uint8_t(n >> 16);
				buf[4] = uint8_t(n >> 24);
				_size += 5;
			}
			else if (n == UINT32_MAX - 1)
			{
				buf[0] = 0xFE;
				++_size;
			}
			else
			{
				buf[0] = 0xFF;
				++_size;
			}
		}

		void WriteUint32(uint32_t n)
		{
			WriteData(n);
		}

		void WriteCompressedInt32(int32_t n)
		{
			// Do the zig-zag conversion in unsigned arithmetic.  In particular,
			// negating INT_MIN in signed arithmetic is undefined.
			uint32_t un = (uint32_t)n;
			uint32_t v = (un << 1) ^ (0U - (un >> 31));
			WriteCompressedUint32(v);
		}

		// A deferred custom-attribute type index occupies five bytes while the
		// attribute range is being resolved.  The native reader accepts the F0
		// form even when a shorter representation would have been possible.
		uint32_t WriteCompressedInt32Placeholder()
		{
			uint32_t offset = _size;
			SureRemainSize(5);
			_data[_size++] = 0xF0;
			_data[_size++] = 0;
			_data[_size++] = 0;
			_data[_size++] = 0;
			_data[_size++] = 0;
			return offset;
		}

		void PatchCompressedInt32Placeholder(uint32_t offset, int32_t n)
		{
			if (offset > _size || 5u > _size - offset)
				RaiseExecutionEngineException("custom attribute type fixup offset out of range");
			uint32_t un = (uint32_t)n;
			uint32_t v = (un << 1) ^ (0U - (un >> 31));
			_data[offset] = 0xF0;
			_data[offset + 1] = (uint8_t)v;
			_data[offset + 2] = (uint8_t)(v >> 8);
			_data[offset + 3] = (uint8_t)(v >> 16);
			_data[offset + 4] = (uint8_t)(v >> 24);
		}

		template<typename T>
		void WriteData(T x)
		{
			int32_t n = sizeof(T);
			SureRemainSize(n);
			std::memcpy(_data + _size, &x, n);
			_size += n;
		}

		void WriteBytes(const uint8_t* data, uint32_t len)
		{
			SureRemainSize(len);
			std::memcpy(_data + _size, data, len);
			_size += len;
		}

		void Write(const CustomAttributeDataWriter& writer)
		{
			SureRemainSize(writer._size);
			std::memcpy(_data + _size, writer._data, writer._size);
			_size += writer._size;
		}

		void Write(BlobReader& reader, int32_t count)
		{
			if (count < 0)
				RaiseExecutionEngineException("negative custom attribute copy size");
			const uint32_t length = static_cast<uint32_t>(count);
			// Validate before memcpy; a later checked skip cannot undo an overread.
			if (reader.GetReadPosition() > reader.GetLength() ||
				length > reader.GetLength() - reader.GetReadPosition())
				RaiseExecutionEngineException("custom attribute copy exceeds input blob");
			SureRemainSize(length);
			if (length != 0)
				std::memcpy(_data + _size, reader.GetDataOfReadPosition(), length);
			_size += length;
			reader.SkipBytes(length);
		}

		void PopByte()
		{
			IL2CPP_ASSERT(_size > 0);
			--_size;
		}

		void ReplaceLastByte(byte x)
		{
			IL2CPP_ASSERT(_size > 0);
			_data[_size - 1] = x;
		}

	private:
		uint32_t Round2Exp(uint32_t n)
		{
			uint32_t s = 64;
			while (s < n)
			{
				if (s > UINT32_MAX / 2)
					return n;
				s *= 2;
			}
			return s;
		}

		void SureRemainSize(uint32_t remainSize)
		{
			if (remainSize > UINT32_MAX - _size)
			{
				RaiseExecutionEngineException("custom attribute output size overflow");
			}
			uint32_t newSize = _size + remainSize;
			if (newSize > _capacity)
			{
				Resize(newSize);
			}
		}

		void Resize(uint32_t newSize)
		{
			newSize = Round2Exp(newSize);
			uint8_t* oldData = _data;
			uint8_t* newData = (uint8_t*)HYBRIDCLR_MALLOC(newSize);
			if (newData == nullptr)
			{
				il2cpp::vm::Exception::RaiseOutOfMemoryException();
			}
			std::memcpy(newData, oldData, _size);
			HYBRIDCLR_FREE(oldData);
			_data = newData;
			_capacity = newSize;
		}
	};
}
}
