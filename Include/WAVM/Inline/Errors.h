#pragma once

#include "WAVM/Platform/Diagnostics.h"

#include <cstdarg>

#define WAVM_UNREACHABLE()                                                                         \
	while(true) { WAVM_DEBUG_TRAP(); };

namespace WAVM { namespace Errors {
	// Fatal error handling.
	[[noreturn]] inline void fatalfWithCallStack(const char* messageFormat, ...)
	{
		va_list varArgs;
		va_start(varArgs, messageFormat);
		Platform::handleFatalError(messageFormat, true, varArgs);
	}
	[[noreturn]] inline void fatalf(const char* messageFormat, ...)
	{
		va_list varArgs;
		va_start(varArgs, messageFormat);
		Platform::handleFatalError(messageFormat, false, varArgs);
	}
	[[noreturn]] inline void fatal(const char* message) { fatalf("%s", message); }

	[[noreturn]] inline void unimplemented(const char* context)
	{
		fatalf("%s is unimplemented", context);
	}
}}
