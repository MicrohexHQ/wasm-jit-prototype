#include <stdint.h>
#include <cmath>
#include <string>

#include "Lexer.h"
#include "Parse.h"
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Inline/Errors.h"
#include "WAVM/Inline/FloatComponents.h"
#include "WAVM/Platform/Defines.h"
#include "WAVM/Platform/Mutex.h"

using namespace WAVM;
using namespace WAVM::WAST;

// Mutexes used by gdtoa.
static Platform::Mutex gdtoaMutexes[2];
extern "C" void ACQUIRE_DTOA_LOCK(unsigned int index) { gdtoaMutexes[index].lock(); }
extern "C" void FREE_DTOA_LOCK(unsigned int index) { gdtoaMutexes[index].unlock(); }
extern "C" unsigned int dtoa_get_threadno()
{
	// Returning 0 works because we never set the number of threads, so gdtoa just falls back to
	// using a global freelist protected by ACQUIRE_DTOA_LOCK/FREE_DTOA_LOCK.
	return 0;
}

// Defined in the WAVM/ThirdParty/gdtoa library.
extern "C" float gdtoa_strtof(const char* s, char** p);
extern "C" double gdtoa_strtod(const char* s, char** p);

// Parses an optional + or - sign and returns true if a - sign was parsed.
// If either a + or - sign is parsed, nextChar is advanced past it.
static bool parseSign(const char*& nextChar)
{
	if(*nextChar == '-')
	{
		++nextChar;
		return true;
	}
	else if(*nextChar == '+')
	{
		++nextChar;
	}
	return false;
}

// Parses an unsigned integer from hexits, starting with "0x", and advancing nextChar past the
// parsed hexits. be called for input that's already been accepted by the lexer as a hexadecimal
// integer.
static U64 parseHexUnsignedInt(const char*& nextChar, ParseState* parseState, U64 maxValue)
{
	const char* firstHexit = nextChar;
	wavmAssert(nextChar[0] == '0' && (nextChar[1] == 'x' || nextChar[1] == 'X'));
	nextChar += 2;

	U64 result = 0;
	U8 hexit = 0;
	while(true)
	{
		if(*nextChar == '_')
		{
			++nextChar;
			continue;
		}
		if(!tryParseHexit(nextChar, hexit)) { break; }
		if(result > (maxValue - hexit) / 16)
		{
			parseErrorf(parseState, firstHexit, "integer literal is too large");
			result = maxValue;
			while(tryParseHexit(nextChar, hexit)) {};
			break;
		}
		wavmAssert(result * 16 + hexit >= result);
		result = result * 16 + hexit;
	}
	return result;
}

// Parses an unsigned integer from digits, advancing nextChar past the parsed digits.
// Assumes it will only be called for input that's already been accepted by the lexer as a decimal
// integer.
static U64 parseDecimalUnsignedInt(const char*& nextChar,
								   ParseState* parseState,
								   U64 maxValue,
								   const char* context)
{
	U64 result = 0;
	const char* firstDigit = nextChar;
	while(true)
	{
		if(*nextChar == '_')
		{
			++nextChar;
			continue;
		}
		if(*nextChar < '0' || *nextChar > '9') { break; }

		const U8 digit = *nextChar - '0';
		++nextChar;

		if(result > U64(maxValue - digit) / 10)
		{
			parseErrorf(parseState, firstDigit, "%s is too large", context);
			result = maxValue;
			while((*nextChar >= '0' && *nextChar <= '9') || *nextChar == '_') { ++nextChar; };
			break;
		}
		wavmAssert(result * 10 + digit >= result);
		result = result * 10 + digit;
	};
	return result;
}

// Parses a floating-point NaN, advancing nextChar past the parsed characters.
// Assumes it will only be called for input that's already been accepted by the lexer as a literal
// NaN.
template<typename Float> Float parseNaN(const char*& nextChar, ParseState* parseState)
{
	const char* firstChar = nextChar;

	typedef FloatComponents<Float> FloatComponents;
	FloatComponents resultComponents;
	resultComponents.bits.sign = parseSign(nextChar) ? 1 : 0;
	resultComponents.bits.exponent = FloatComponents::maxExponentBits;

	wavmAssert(nextChar[0] == 'n' && nextChar[1] == 'a' && nextChar[2] == 'n');
	nextChar += 3;

	if(*nextChar == ':')
	{
		++nextChar;

		const U64 significandBits
			= parseHexUnsignedInt(nextChar, parseState, FloatComponents::maxSignificand);
		if(!significandBits)
		{
			parseErrorf(parseState, firstChar, "NaN significand must be non-zero");
			resultComponents.bits.significand = 1;
		}
		resultComponents.bits.significand = typename FloatComponents::Bits(significandBits);
	}
	else
	{
		// If the NaN's significand isn't specified, just set the top bit.
		resultComponents.bits.significand = typename FloatComponents::Bits(1)
											<< (FloatComponents::numSignificandBits - 1);
	}

	return resultComponents.value;
}

// Parses a floating-point infinity. Does not advance nextChar.
// Assumes it will only be called for input that's already been accepted by the lexer as a literal
// infinity.
template<typename Float> Float parseInfinity(const char* nextChar)
{
	// Floating point infinite is represented by max exponent with a zero significand.
	typedef FloatComponents<Float> FloatComponents;
	FloatComponents resultComponents;
	resultComponents.bits.sign = parseSign(nextChar) ? 1 : 0;
	resultComponents.bits.exponent = FloatComponents::maxExponentBits;
	resultComponents.bits.significand = 0;
	return resultComponents.value;
}

template<typename Float> Float parseNonSpecialFloat(const char* firstChar, char** endChar);
template<> F32 parseNonSpecialFloat(const char* firstChar, char** endChar)
{
	return gdtoa_strtof(firstChar, endChar);
}
template<> F64 parseNonSpecialFloat(const char* firstChar, char** endChar)
{
	return gdtoa_strtod(firstChar, endChar);
}

// Parses a floating point literal, advancing nextChar past the parsed characters. Assumes it will
// only be called for input that's already been accepted by the lexer as a float literal.
template<typename Float> Float parseFloat(const char*& nextChar, ParseState* parseState)
{
	// Scan the token's characters for underscores, and make a copy of it without the underscores
	// for strtof/strtod.
	const char* firstChar = nextChar;
	std::string noUnderscoreString;
	bool hasUnderscores = false;
	while(true)
	{
		const char c = *nextChar;

		// Determine whether the next character is still part of the number.
		const bool isNumericChar = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
								   || (c >= 'A' && c <= 'F') || c == 'x' || c == 'X' || c == 'p'
								   || c == 'P' || c == '+' || c == '-' || c == '.' || c == '_';
		if(!isNumericChar) { break; }

		if(c == '_' && !hasUnderscores)
		{
			// If this is the first underscore encountered, copy the preceding characters of the
			// number to a std::string.
			noUnderscoreString = std::string(firstChar, nextChar);
			hasUnderscores = true;
		}
		else if(c != '_' && hasUnderscores)
		{
			// If an underscore has previously been encountered, copy non-underscore characters to
			// that string.
			noUnderscoreString += *nextChar;
		}

		++nextChar;
	};

	// Pass the underscore-free string to parseNonSpecialF64 instead of the original input string.
	const char* noUnderscoreFirstChar = firstChar;
	if(hasUnderscores) { noUnderscoreFirstChar = noUnderscoreString.c_str(); }

	// Use David Gay's strtof/strtod to parse a floating point number.
	char* endChar = nullptr;
	Float result = parseNonSpecialFloat<Float>(noUnderscoreFirstChar, &endChar);
	if(endChar == noUnderscoreFirstChar)
	{ Errors::fatalf("strtof/strtod failed to parse number accepted by lexer"); }

	if(std::isinf(result)) { parseErrorf(parseState, firstChar, "float literal is too large"); }

	return result;
}

// Tries to parse an numeric literal token as an integer, advancing cursor->nextToken.
// Returns true if it matched a token.
template<typename UnsignedInt>
bool tryParseInt(CursorState* cursor,
				 UnsignedInt& outUnsignedInt,
				 I64 minSignedValue,
				 U64 maxUnsignedValue)
{
	bool isNegative = false;
	U64 u64 = 0;

	const char* nextChar = cursor->parseState->string + cursor->nextToken->begin;
	switch(cursor->nextToken->type)
	{
	case t_decimalInt:
		isNegative = parseSign(nextChar);
		u64 = parseDecimalUnsignedInt(nextChar,
									  cursor->parseState,
									  isNegative ? -U64(minSignedValue) : maxUnsignedValue,
									  "int literal");
		break;
	case t_hexInt:
		isNegative = parseSign(nextChar);
		u64 = parseHexUnsignedInt(
			nextChar, cursor->parseState, isNegative ? -U64(minSignedValue) : maxUnsignedValue);
		break;
	default: return false;
	};

	if(minSignedValue == 0 && isNegative)
	{
		isNegative = false;
		outUnsignedInt = 0;
		return false;
	}
	else
	{
		outUnsignedInt = isNegative ? UnsignedInt(-u64) : UnsignedInt(u64);

		++cursor->nextToken;
		wavmAssert(nextChar <= cursor->parseState->string + cursor->nextToken->begin);

		return true;
	}
}

// Tries to parse a numeric literal literal token as a float, advancing cursor->nextToken.
// Returns true if it matched a token.
template<typename Float> bool tryParseFloat(CursorState* cursor, Float& outFloat)
{
	const char* nextChar = cursor->parseState->string + cursor->nextToken->begin;
	switch(cursor->nextToken->type)
	{
	case t_decimalInt:
	case t_decimalFloat: outFloat = parseFloat<Float>(nextChar, cursor->parseState); break;
	case t_hexInt:
	case t_hexFloat: outFloat = parseFloat<Float>(nextChar, cursor->parseState); break;
	case t_floatNaN: outFloat = parseNaN<Float>(nextChar, cursor->parseState); break;
	case t_floatInf: outFloat = parseInfinity<Float>(nextChar); break;
	default:
		parseErrorf(cursor->parseState, cursor->nextToken, "expected float literal");
		return false;
	};

	++cursor->nextToken;
	wavmAssert(nextChar <= cursor->parseState->string + cursor->nextToken->begin);

	return true;
}

bool WAST::tryParseU64(CursorState* cursor, U64& outI64)
{
	return tryParseInt<U64>(cursor, outI64, 0, UINT64_MAX);
}

bool WAST::tryParseUptr(CursorState* cursor, Uptr& outUptr)
{
	return tryParseInt<Uptr>(cursor, outUptr, 0, UINTPTR_MAX);
}

U32 WAST::parseU32(CursorState* cursor)
{
	U32 result;
	if(!tryParseInt<U32>(cursor, result, 0, UINT32_MAX))
	{
		parseErrorf(cursor->parseState, cursor->nextToken, "expected u32 literal");
		throw RecoverParseException();
	}
	return result;
}

I8 WAST::parseI8(CursorState* cursor)
{
	U32 result;
	if(!tryParseInt<U32>(cursor, result, INT8_MIN, UINT8_MAX))
	{
		parseErrorf(cursor->parseState, cursor->nextToken, "expected i8 literal");
		throw RecoverParseException();
	}
	return I8(result);
}

I16 WAST::parseI16(CursorState* cursor)
{
	U32 result;
	if(!tryParseInt<U32>(cursor, result, INT16_MIN, UINT16_MAX))
	{
		parseErrorf(cursor->parseState, cursor->nextToken, "expected i16 literal");
		throw RecoverParseException();
	}
	return I16(result);
}

I32 WAST::parseI32(CursorState* cursor)
{
	U32 result;
	if(!tryParseInt<U32>(cursor, result, INT32_MIN, UINT32_MAX))
	{
		parseErrorf(cursor->parseState, cursor->nextToken, "expected i32 literal");
		throw RecoverParseException();
	}
	return I32(result);
}

I64 WAST::parseI64(CursorState* cursor)
{
	U64 result;
	if(!tryParseInt<U64>(cursor, result, INT64_MIN, UINT64_MAX))
	{
		parseErrorf(cursor->parseState, cursor->nextToken, "expected i64 literal");
		throw RecoverParseException();
	}
	return I64(result);
}

F32 WAST::parseF32(CursorState* cursor)
{
	F32 result;
	if(!tryParseFloat(cursor, result))
	{
		parseErrorf(cursor->parseState, cursor->nextToken, "expected f32 literal");
		throw RecoverParseException();
	}
	return result;
}

F64 WAST::parseF64(CursorState* cursor)
{
	F64 result;
	if(!tryParseFloat(cursor, result))
	{
		parseErrorf(cursor->parseState, cursor->nextToken, "expected f64 literal");
		throw RecoverParseException();
	}
	return result;
}

V128 WAST::parseV128(CursorState* cursor)
{
	V128 result;
	switch(cursor->nextToken->type)
	{
	case t_i8x16:
		++cursor->nextToken;
		for(Uptr laneIndex = 0; laneIndex < 16; ++laneIndex)
		{ result.i8[laneIndex] = parseI8(cursor); }
		break;
	case t_i16x8:
		++cursor->nextToken;
		for(Uptr laneIndex = 0; laneIndex < 8; ++laneIndex)
		{ result.i16[laneIndex] = parseI16(cursor); }
		break;
	case t_i32x4:
		++cursor->nextToken;
		for(Uptr laneIndex = 0; laneIndex < 4; ++laneIndex)
		{ result.i32[laneIndex] = parseI32(cursor); }
		break;
	case t_i64x2:
		++cursor->nextToken;
		for(Uptr laneIndex = 0; laneIndex < 2; ++laneIndex)
		{ result.i64[laneIndex] = parseI64(cursor); }
		break;
	case t_f32x4:
		++cursor->nextToken;
		for(Uptr laneIndex = 0; laneIndex < 4; ++laneIndex)
		{ result.f32[laneIndex] = parseF32(cursor); }
		break;
	case t_f64x2:
		++cursor->nextToken;
		for(Uptr laneIndex = 0; laneIndex < 2; ++laneIndex)
		{ result.f64[laneIndex] = parseF64(cursor); }
		break;
	default:
		parseErrorf(cursor->parseState,
					cursor->nextToken,
					"expected 'i8x6', 'i16x8', 'i32x4', 'i64x2', 'f32x4', or 'f64x2'");
		throw RecoverParseException();
	};

	return result;
}
