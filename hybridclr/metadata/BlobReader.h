#pragma once

#include "../CommonDef.h"
#include "BlobReaderBounds.h"
#include "MetadataUtil.h"

namespace hybridclr
{
namespace metadata
{

    class BlobReader
    {
    public:
        BlobReader(const byte* buf, uint32_t length) : _buf(buf), _length(length), _readPos(0)
        {

        }

        const byte* GetData() const
        {
            return _buf;
        }

        uint32_t GetLength() const
        {
            return _length;
        }

        uint32_t GetReadPosition() const
        {
            return _readPos;
        }

        const byte* GetDataOfReadPosition() const
        {
            if (_buf == nullptr)
            {
                return nullptr;
            }
            if (!RequireAvailable(0))
            {
                return nullptr;
            }
            return _buf + _readPos;
        }

        bool IsEmpty() const
        {
            return _readPos >= _length;
        }

        bool NonEmpty() const
        {
            return _readPos < _length;
        }

        int32_t ReadCompressedInt32()
        {
            uint32_t unsignedValue = ReadCompressedUint32();
            uint32_t value = unsignedValue >> 1;
            if (!(unsignedValue & 0x1))
            {
                return value;
            }
            if (value < 0x40)
            {
                return value - 0x40;
            }
            if (value < 0x2000)
            {
                return value - 0x2000;
            }
            if (value < 0x10000000)
            {
                return value - 0x10000000;
            }
            IL2CPP_ASSERT(value < 0x20000000);
            return value - 0x20000000;
        }

        static uint32_t ReadCompressedUint32(const byte* buf, uint32_t& lengthSize)
        {
            uint32_t firstByte = buf[0];
            if (firstByte < 128)
            {
                lengthSize = 1;
                return firstByte;
            }
            else if (firstByte < 192)
            {
                lengthSize = 2;
                return ((firstByte & 0x3f) << 8) | buf[1];
            }
            else if (firstByte < 224)
            {
                lengthSize = 4;
                return ((firstByte & 0x1f) << 24) | (((uint32_t)buf[1]) << 16) | ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
            }
            else
            {
                RaiseExecutionEngineException("bad metadata data. ReadEncodeLength fail");
                return 0;
            }
        }

        static bool TryReadCompressedUint32(const byte* buf, uint32_t available, uint32_t& value, uint32_t& lengthSize)
        {
            return BlobReaderBounds::TryReadCompressedUint32(buf, available, value, lengthSize);
        }

        static bool TryReadLengthPrefixedBlob(const byte* buf, uint32_t available, const byte*& data, uint32_t& length)
        {
            return BlobReaderBounds::TryReadLengthPrefixedBlob(buf, available, data, length);
        }

        uint32_t ReadCompressedUint32()
        {
            if (!RequireAvailable(1))
            {
                return 0;
            }
            uint32_t lengthSize;
            uint32_t value;
            if (!TryReadCompressedUint32(_buf + _readPos, _length - _readPos, value, lengthSize))
            {
                RaiseExecutionEngineException("bad metadata data. BlobReader compressed read out of bounds");
                return 0;
            }
            _readPos += lengthSize;
            return value;
        }

        uint8_t ReadByte()
        {
            if (!RequireAvailable(1))
            {
                return 0;
            }
            return _buf[_readPos++];
        }

        uint16_t Read16()
        {
            if (!RequireAvailable(2))
            {
                return 0;
            }
            uint16_t value = GetU2LittleEndian(_buf + _readPos);
            _readPos += 2;
            return value;
        }

        uint32_t Read32()
        {
            if (!RequireAvailable(4))
            {
                return 0;
            }
            uint32_t value = (uint32_t)GetI4LittleEndian(_buf + _readPos);
            _readPos += 4;
            return value;
        }

        bool TryRead32(uint32_t& value)
        {
            if (HasAvailable(4))
            {
                value = Read32();
                return true;
            }
            return false;
        }

        uint64_t Read64()
        {
            if (!RequireAvailable(8))
            {
                return 0;
            }
            uint64_t value = (uint64_t)GetI8LittleEndian(_buf + _readPos);
            _readPos += 8;
            return value;
        }

        float ReadFloat()
        {
            uint32_t x = Read32();
            return *(float*)&x;
        }

        double ReadDouble()
        {
            uint64_t x = Read64();
            return *(double*)&x;
        }

        //template<typename T>
        //T Read()
        //{
        //    IL2CPP_ASSERT(_readPos + sizeof(T) <= _length);
        //    T value = *(T*)(_buf + _readPos);
        //    _readPos += sizeof(T);
        //    return value;
        //}

        uint8_t PeekByte()
        {
            if (!RequireAvailable(1))
            {
                return 0;
            }
            return _buf[_readPos];
        }

        void SkipByte()
        {
            if (!RequireAvailable(1))
            {
                return;
            }
            ++_readPos;
        }

        void SkipBytes(uint32_t len)
        {
            if (!RequireAvailable(len))
            {
                return;
            }
            _readPos += len;
        }

        const byte* GetAndSkipCurBytes(uint32_t len)
        {
            if (!RequireAvailable(len))
            {
                return nullptr;
            }
            const byte* data = _buf + _readPos;
            _readPos += len;
            return data;
        }

    private:
        bool HasAvailable(uint32_t size) const
        {
            return _buf != nullptr && _readPos <= _length && size <= _length - _readPos;
        }

        bool RequireAvailable(uint32_t size) const
        {
            if (HasAvailable(size))
            {
                return true;
            }
            RaiseExecutionEngineException("bad metadata data. BlobReader read out of bounds");
            return false;
        }

        const byte* const _buf;
        const uint32_t _length;
        uint32_t _readPos;
    };

}
}
