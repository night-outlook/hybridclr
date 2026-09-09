#include "RuntimeConfig.h"

#include "vm/Exception.h"
#include "vm/AssemblyShadowTypes.h"

namespace hybridclr
{
	static int32_t s_threadObjectStackSize = 1024 * 128;
	static int32_t s_threadFrameStackSize = 1024 * 2;
	static int32_t s_threadExceptionFlowSize = 512;
	static int32_t s_maxMethodBodyCacheSize = 1024;
	static int32_t s_maxMethodInlineDepth = 3;
	static int32_t s_maxInlineableMethodBodySize = 32;



	int32_t RuntimeConfig::GetRuntimeOption(RuntimeOptionId optionId)
	{
		switch (optionId)
		{	
		case RuntimeOptionId::InterpreterThreadObjectStackSize:
			return s_threadObjectStackSize;
		case RuntimeOptionId::InterpreterThreadFrameStackSize:
			return s_threadFrameStackSize;
		case RuntimeOptionId::InterpreterThreadExceptionFlowSize:
			return s_threadExceptionFlowSize;
		case RuntimeOptionId::MaxMethodBodyCacheSize:
			return s_maxMethodBodyCacheSize;
		case RuntimeOptionId::MaxMethodInlineDepth:
			return s_maxMethodInlineDepth;
		case RuntimeOptionId::MaxInlineableMethodBodySize:
			return s_maxInlineableMethodBodySize;
		case RuntimeOptionId::AssemblyShadowMetadataBudgetCapabilityVersion:
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
			return il2cpp::vm::kAssemblyShadowMetadataBudgetCapabilityVersion;
#else
			return 0;
#endif
		case RuntimeOptionId::AssemblyShadowRecoveryCapabilityVersion:
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
			return il2cpp::vm::kAssemblyShadowRecoveryCapabilityVersion;
#else
			return 0;
#endif
		default:
		{
			TEMP_FORMAT(optionIdStr, "%d", optionId);
			il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetArgumentException(optionIdStr, "invalid runtime option id"));
			return 0;
		}
		}
	}

	void RuntimeConfig::SetRuntimeOption(RuntimeOptionId optionId, int32_t value)
	{
		switch (optionId)
		{
		case hybridclr::RuntimeOptionId::InterpreterThreadObjectStackSize:
			s_threadObjectStackSize = value;
			break;
		case hybridclr::RuntimeOptionId::InterpreterThreadFrameStackSize:
			s_threadFrameStackSize = value;
			break;
		case hybridclr::RuntimeOptionId::InterpreterThreadExceptionFlowSize:
			s_threadExceptionFlowSize = value;
			break;
		case RuntimeOptionId::MaxMethodBodyCacheSize:
			s_maxMethodBodyCacheSize = value;
			break;
		case RuntimeOptionId::MaxMethodInlineDepth:
			s_maxMethodInlineDepth = value;
			break;
		case RuntimeOptionId::MaxInlineableMethodBodySize:
			s_maxInlineableMethodBodySize = value;
			break;
		case RuntimeOptionId::AssemblyShadowMetadataBudgetCapabilityVersion:
		case RuntimeOptionId::AssemblyShadowRecoveryCapabilityVersion:
		{
			TEMP_FORMAT(optionIdStr, "%d", optionId);
			il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetArgumentException(optionIdStr, "read-only runtime option id"));
			break;
		}
		default:
		{
			TEMP_FORMAT(optionIdStr, "%d", optionId);
			il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetArgumentException(optionIdStr, "invalid runtime option id"));
			break;
		}
		}
	}

	uint32_t RuntimeConfig::GetInterpreterThreadObjectStackSize()
	{
		return s_threadObjectStackSize;
	}

	uint32_t RuntimeConfig::GetInterpreterThreadFrameStackSize()
	{
		return s_threadFrameStackSize;
	}

	uint32_t RuntimeConfig::GetInterpreterThreadExceptionFlowSize()
	{
		return s_threadExceptionFlowSize;
	}

	int32_t RuntimeConfig::GetMaxMethodBodyCacheSize()
	{
		return s_maxMethodBodyCacheSize;
	}

	int32_t RuntimeConfig::GetMaxMethodInlineDepth()
	{
		return s_maxMethodInlineDepth;
	}

	int32_t RuntimeConfig::GetMaxInlineableMethodBodySize()
	{
		return s_maxInlineableMethodBodySize;
	}

}
