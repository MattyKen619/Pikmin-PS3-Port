#ifndef _TYPES_H
#define _TYPES_H

// For compiling code copy-pasted from Ghidra (TODO: remove this)
typedef unsigned int uint;

// Standard types
#if defined(_MSC_VER)
typedef signed __int8 s8;
typedef signed __int16 s16;
typedef signed __int32 s32;
typedef signed __int64 s64;
typedef unsigned __int8 u8;
typedef unsigned __int16 u16;
typedef unsigned __int32 u32;
typedef unsigned __int64 u64;
#elif defined(__LP64__)
#define TYPES_32_ARE_INT 1
// 64-bit-long targets (e.g. the PS3 PPU with the ps3dev toolchain): `long` is 64 bits there, so the
// 32-bit types must be based on `int` to keep struct layouts and file formats intact.
typedef signed char s8;
typedef signed short s16;
typedef signed int s32;
typedef signed long long s64;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
#else
typedef signed char s8;
typedef signed short s16;
typedef signed long s32;
typedef signed long long s64;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;
typedef unsigned long long u64;
#endif

#ifndef TYPES_32_ARE_INT
#define TYPES_32_ARE_INT 0
#endif

// Pointer-sized unsigned integer: 32 bits on GameCube, 64 bits on the PS3 (LP64).
// Used for pointer<->integer casts, e.g. `(u32)(uptr)ptr`. Identical to `u32` for the matching build.
#if defined(__UINTPTR_TYPE__)
typedef __UINTPTR_TYPE__ uptr;
#else
typedef u32 uptr;
#endif

// PTR32(T): a pointer field that is always 4 bytes, for structures laid directly over data loaded from
// game files (whose layouts store 32-bit pointers/offsets). CodeWarrior (and any 32-bit target) gets a
// plain `T*`. On LP64 targets such as the PS3, `Ptr32<T>` stores the address in 32 bits; PS3 user
// addresses fit in 32 bits, so no information is lost (PSL1GHT uses the same idea for system structs).
#if defined(__cplusplus) && defined(__LP64__)
template <typename T>
struct Ptr32 {
	Ptr32() = default;
	Ptr32(T* ptr)
	    : mAddr((u32)(uptr)ptr)
	{
	}
	Ptr32& operator=(T* ptr)
	{
		mAddr = (u32)(uptr)ptr;
		return *this;
	}
	operator T*() const { return (T*)(uptr)mAddr; }
	T* operator->() const { return (T*)(uptr)mAddr; }
	T* get() const { return (T*)(uptr)mAddr; }

	u32 mAddr;
};
#define PTR32(...) Ptr32<__VA_ARGS__>
#else
#define PTR32(...) __VA_ARGS__*
#endif

// Volatile types
typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;
typedef volatile u64 vu64;
typedef volatile s8 vs8;
typedef volatile s16 vs16;
typedef volatile s32 vs32;
typedef volatile s64 vs64;

typedef float f32;
typedef double f64;
typedef long double f128;
typedef volatile f32 vf32;
typedef volatile f64 vf64;
typedef volatile f128 vf128;

// A boolean value typedef inspired by the Win32 API.
typedef int BOOL;
#undef TRUE
#define TRUE (1)
#undef FALSE
#define FALSE (0)

// These math constants were seeminly defined somewhere because Nintendo used them often.
#define TAU        6.2831855f
#define PI         3.1415927f
#define HALF_PI    1.5707964f
#define THIRD_PI   1.0471976f
#define QUARTER_PI 0.7853982f

// When compiling sysCore.dll under MSVC, we define the macro `SYSCORE_EXPORTS` to
// seamlessly make the switch from `dllimport` to `dllexport` across the codebase.
//
// `__declspec(dllimport)` is what makes the compiler emit IAT-indirect symbols prefixed
// with `__imp_`.  It's an ongoing TODO to catch all of the places where the `SYSCORE_API`
// decorator is needed, but many of the hot exported types are already taken care of.  If
// you are nonmatching due to `call [__imp_...]` or `mov r,[__imp_...]`, we missed a spot.
#if defined(_MSC_VER)
#if defined(SYSCORE_EXPORTS)
#define SYSCORE_API __declspec(dllexport)
#else
#define SYSCORE_API __declspec(dllimport)
#endif
#else
// On MetroWerks, everything was statically linked, so this macro expands to nothing.
#define SYSCORE_API
#endif

// Here we check if the decomp build system is configured for a non-matching build, and define `BUILD_MATCHING` if it is not.
// Please do not use the `DTK_CONFIG_NONMATCHING` macro for any other purpose, and instead check if the the `BUILD_MATCHING`
// macro is defined.  This is to ensure the codebase doesn't require any preprocessor macros to be defined by the build system.
#if defined(DTK_CONFIG_NONMATCHING) && !DTK_CONFIG_NONMATCHING
#define BUILD_MATCHING
#endif

// The following version group macro is for conditionally-compiled code shared among Japanese retail versions.
#if defined(VERSION_GPIJ01_01) || defined(VERSION_GPIJ01_02)
#define VERSION_GPIJ01
#endif

// The following version group macro is for conditionally-compiled code shared among American retail versions.
#if defined(VERSION_GPIE01_00) || defined(VERSION_GPIE01_01)
#define VERSION_GPIE01
#endif

// The following version group macro is for conditionally-compiled code shared among European retail versions.
#if defined(VERSION_GPIP01_00) // A bit redundant, but I want consistency.
#define VERSION_GPIP01
#endif

// The following version group macro is for conditionally-compiled code shared among all three demo versions.
#if defined(VERSION_DPIJ01_PIKIDEMO) || defined(VERSION_G98E01_PIKIDEMO) || defined(VERSION_G98P01_PIKIDEMO)
#define VERSION_PIKIDEMO
#endif

// Defining this macro enables fixes for known crashes and mistakes.
#if !defined(BUILD_MATCHING) && !defined(BUGFIX)
#define BUGFIX
#endif

// Defining this macro enables development features removed from the final game.
#if !defined(BUILD_MATCHING) && !defined(DEVELOP)
// #define DEVELOP
#endif

// The value of this macro controls the conditional compilation of all uses for JAudio (audio library) in the codebase.
#ifndef PIKI_USE_JAUDIO
#define PIKI_USE_JAUDIO TRUE
#endif

// The value of this macro controls the conditional compilation of all uses for Dolphin GX (graphics API) in the codebase.
#ifndef PIKI_USE_DGX
#define PIKI_USE_DGX TRUE
#endif

// MSVC 6.0 parses empty macro arguments in a non-standard way, skipping that parameter instead of reading it as nothing.
#define MACRO_NOTHING

// Sneaky way to put top-level commas in macro arguments.  Useful for `MISSING_ARGUMENT` macro in "DebugLog.h"
#define MACRO_COMMA ,

// Your tyical stringification macros
#define TO_STRING(x) STRINGIFY(x)
#define STRINGIFY(x) #x

// Workaround to introduce scoped enums (A feature of C++11 and onward).
#define BEGIN_ENUM_TYPE(name) \
	struct name {             \
		typedef

#define END_ENUM_TYPE \
	Type;             \
	}

// Do you ever get tired of typing this?  Well do I have good news for you:
#ifdef __cplusplus
#define BEGIN_SCOPE_EXTERN_C extern "C" {
#else
#define BEGIN_SCOPE_EXTERN_C
#endif

#ifdef __cplusplus
#define END_SCOPE_EXTERN_C }
#else
#define END_SCOPE_EXTERN_C
#endif

// For bugfixes that can fit sub-statement, use this instead of a clunky #if #else #endif.
#if defined(BUGFIX)
#define TERNARY_BUGFIX(fixed, buggy) fixed
#else
#define TERNARY_BUGFIX(fixed, buggy) buggy
#endif

// For develop features that can fit sub-statement, use this instead of a clunky #if #else #endif.
#if defined(DEVELOP)
#define TERNARY_DEVELOP(development, release) development
#else
#define TERNARY_DEVELOP(development, release) release
#endif

// For undesireable code necessary to produce a matching build, use this instead of a clunky #if #else #endif.
#if defined(BUILD_MATCHING)
#define TERNARY_BUILD_MATCHING(matching, nonmatching) matching
#else
#define TERNARY_BUILD_MATCHING(matching, nonmatching) nonmatching
#endif

// We yearn for modernity.
#if defined(__cplusplus) && __cplusplus < 201103L
#define nullptr 0
#endif

// Here's some fun facts about the non-standard C++ that `immut` and `NRef` exist to eliminate:
//  - According to https://isocpp.org/wiki/faq/cpp11-language#rval, binding rvalues to non-const references has been forbidden since C++98.
//  - IBM's C/C++ compiler has the option `-qlanglvl=compatrvaluebinding`, for which its documentation says "the C++ Standard (2003)
//    indicates that an rvalue can only be bound to a const lvalue reference".
//  - MSVC allows rvalues to bind to non-const references by default.  However, by specifying the `/Zc:referenceBinding` option, "the
//    compiler follows section 8.5.3 of the C++11 standard".
// So was it C++98, 03, or 11?  Nobody can agree on what / when to blame, but MWCC 1.2.5 is definitely a standards non-compliant compiler.

// This entire codebase is cripplingly const-incorrect.  The result of a constructor being bound to a non-const reference in function
// parameters is done all the time, not to mention the mutable string literals.  Part of the promise of a matching decomp is that the
// original codebase is represented as closely as possible, warts and all, but portability (MWCC 1.2.5 was the *last* version of MWCC
// to allow this non-standard behavior) is also desireable.  Luckily, almost all const-incorrectness in the codebase is merely a result
// of apathy, so this cv-qualifier macro exists to document and fix the places that could have been const-correct but weren't.
#define immut TERNARY_BUILD_MATCHING(MACRO_NOTHING, const)

// Nakata had a bad habit of writing mutable references to lifetime-extended rvalues when a value type would have sufficed, so this macro
// is named for him.  MWCC 1.2.5 sometimes optimizes `Type foo = Type(...)` *really* poorly compared to `Type foo(...)`, so unless you are
// using a different compiler, this const-correctness fix might generate worse code.
#define NRef TERNARY_BUILD_MATCHING(&, MACRO_NOTHING)

// In early revisions of Pikmin 1, Ogawa was confused on which enum he was supposed to use for `SeSystem::playSysSe`/`stopSysSe`.
#if defined(VERSION_PIKIDEMO) || defined(VERSION_GPIJ01)
#define ogEnumFix(right, wrong) TERNARY_BUGFIX(right, wrong)
#else
#define ogEnumFix(right, wrong) right
#endif

// Random and useful macros
#define PATH_MAX  (256)                     // Max path length
#define MAX(a, b) (((a) > (b)) ? (a) : (b)) // Returns the maximum of a and b
#define MIN(a, b) (((a) < (b)) ? (a) : (b)) // Returns the minimum of a and b

// Flag manipulation macros
#define ARRAY_SIZE(o)        (sizeof((o)) / sizeof(*(o)))     // Array size define
#define ALIGN_PREV(X, N)     ((X) & ~((N) - 1))               // Align X to the previous N bytes (N must be power of two)
#define ALIGN_NEXT(X, N)     ALIGN_PREV(((X) + ((N) - 1)), N) // Align X to the next N bytes (N must be power of two)
#define IS_NOT_ALIGNED(X, N) (((X) & ((N) - 1)) != 0)         // True if X is not aligned to N bytes, else false

// Align object to num bytes (num should be power of two)
#if defined(_MSC_VER)
// MSVC 6.0 has no trailing alignment attribute; its __declspec(align(#)) must precede the declarator.
// Also, nothing that uses this macro is byte-aligned in the DLL, so it must have been inert on MSVC.
#define ATTRIBUTE_ALIGN(num)
#else
#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))
#endif

// Makes the compiler averse to using that register for the entire function.
#if defined(__MWERKS__) && defined(BUILD_MATCHING)
#define BUMP_REGISTER(reg) { asm { mr reg, reg } }((void)0)
#else
#define BUMP_REGISTER(reg) ((void)0)
#endif

// clang-format off
#define REPEAT1(x)  x
#define REPEAT2(x)  REPEAT1(x); x
#define REPEAT3(x)  REPEAT2(x); x
#define REPEAT4(x)  REPEAT3(x); x
#define REPEAT5(x)  REPEAT4(x); x
#define REPEAT6(x)  REPEAT5(x); x
#define REPEAT7(x)  REPEAT6(x); x
#define REPEAT8(x)  REPEAT7(x); x
#define REPEAT9(x)  REPEAT8(x); x
#define REPEAT10(x) REPEAT9(x); x
#define REPEAT11(x) REPEAT10(x); x
#define REPEAT12(x) REPEAT11(x); x
#define REPEAT13(x) REPEAT12(x); x
#define REPEAT14(x) REPEAT13(x); x
#define REPEAT15(x) REPEAT14(x); x
#define REPEAT16(x) REPEAT15(x); x
// clang-format on

#define REPEAT(x, n) REPEAT##n(x)

// An empty-bodied (and pure) if-statement gets elided even by debug MSVC, however the registers
// that would have been used to evaluate the condition still get skipped in regalloc.  The number
// of registers to skip (`n`) is modulo 3 since there are only three in the cycle (EAX, ECX, EDX).
#if defined(_MSC_VER) && defined(BUILD_MATCHING)
#define MSVC_CYCLE_REGISTERS(n) REPEAT(if (0)((void)0), n)
#else
#define MSVC_CYCLE_REGISTERS(n) ((void)0)
#endif

// Somehow this overwhelms the automatic inlining score and stops unwanted function inlining
#if defined(__MWERKS__) && defined(BUILD_MATCHING)
#define FORCE_DONT_INLINE REPEAT16(REPEAT10((void*)0))
#else
#define FORCE_DONT_INLINE ((void)0)
#endif

// Add an unused local variable to pad the stack by some number of words
#if defined(__MWERKS__) && defined(BUILD_MATCHING)
#define STACK_PAD_VAR(n) \
	do {                 \
		int pad[n];      \
	} while (0)
#else
#define STACK_PAD_VAR(n) ((void)0)
#endif

// Create a temporary struct to pad the stack by some number of words
#if defined(__MWERKS__) && defined(BUILD_MATCHING)
#define STACK_PAD_STRUCT(n)      \
	if (0)                       \
		(struct { int pad[n]; }) \
		{                        \
		}
#else
#define STACK_PAD_STRUCT(n) ((void)0)
#endif

// Add an unused variable in an inline function to pad the stack by some number of words
#if defined(__MWERKS__) && defined(BUILD_MATCHING)
inline void padStack(void)
{
	int pad = 0;
}
#define STACK_PAD_INLINE(n) REPEAT(padStack(), n)
#else
#define STACK_PAD_INLINE(n) ((void)0)
#endif

// Uses a ternary to pad the stack by some number of words
#if defined(__MWERKS__) && defined(BUILD_MATCHING)
#define STACK_PAD_TERNARY(expr, n) REPEAT((expr) ? "fake" : "fake", n)
#else
#define STACK_PAD_TERNARY(expr, n) ((void)0)
#endif

// Metrowerks C/C++ language extensions
#if defined(__MWERKS__)
#define AT_ADDRESS(addr) : (addr)
#define WEAK            __declspec(weak)
#define DECL_SECT(name) __declspec(section name)
#define ASM             asm
#else
#define AT_ADDRESS(addr)
#define WEAK
#define DECL_SECT(name)
#define ASM
#endif

#define INIT  DECL_SECT(".init")
#define CTORS DECL_SECT(".ctors")

// If an unimplemented function is used by mistake (e.g. in modding), we should trap on that.
#if defined(__MWERKS__) // clang-format off
#define TRAP_UNIMPLEMENTED asm { trap } ((void)0)
#elif defined(_MSC_VER)
#define TRAP_UNIMPLEMENTED __asm { ud2 } __assume(0)
#elif defined(__GNUC__)
#define TRAP_UNIMPLEMENTED __builtin_trap()
#else
#define TRAP_UNIMPLEMENTED ((void)0) /* Replace me */
#endif // clang-format on

// Documenting MetroWerks intrinsic functions for PowerPC and placing them behind macros for portability.
#if defined(__MWERKS__)
#define __mwerks_eieio()                  /* void   */ __eieio()                  //
#define __mwerks_sync()                   /* void   */ __sync()                   //
#define __mwerks_isync()                  /* void   */ __isync()                  //
#define __mwerks_abs(value)               /* int    */ __abs(value)               // int
#define __mwerks_labs(value)              /* long   */ __labs(value)              // long
#define __mwerks_fabs(value)              /* float  */ __fabs(value)              // float
#define __mwerks_fnabs(value)             /* float  */ __fnabs(value)             // float
#define __mwerks_cntlzw(value)            /* int    */ __cntlzw(value)            // int
#define __mwerks_lhbrx(base, idx)         /* int    */ __lhbrx(base, idx)         // void*, int
#define __mwerks_lwbrx(base, idx)         /* int    */ __lwbrx(base, idx)         // void*, int
#define __mwerks_sthbrx(value, base, idx) /* void   */ __sthbrx(value, base, idx) // unsigned short, void*, int
#define __mwerks_stwbrx(value, base, idx) /* void   */ __stwbrx(value, base, idx) // unsigned int, void*, int
#define __mwerks_dcbf(base, offset)       /* void   */ __dcbf(base, offset)       // void*, int
#define __mwerks_dcbt(base, offset)       /* void   */ __dcbt(base, offset)       // void*, int
#define __mwerks_dcbst(base, offset)      /* void   */ __dcbst(base, offset)      // void*, int
#define __mwerks_dcbtst(base, offset)     /* void   */ __dcbtst(base, offset)     // void*, int
#define __mwerks_dcbz(base, offset)       /* void   */ __dcbz(base, offset)       // void*, int
#define __mwerks_mulhw(lhs, rhs)          /* int    */ __mulhw(lhs, rhs)          // int, int
#define __mwerks_mulhwu(lhs, rhs)         /* uint   */ __mulhwu(lhs, rhs)         // uint, uint
#define __mwerks_divw(dividend, divisor)  /* int    */ __divw(dividend, divisor)  // int, int
#define __mwerks_divwu(dividend, divisor) /* uint   */ __divwu(dividend, divisor) // uint, uint
#define __mwerks_fmadd(p1, p2, p3)        /* double */ __fmadd(p1, p2, p3)        // double, double, double
#define __mwerks_fmsub(p1, p2, p3)        /* double */ __fmsub(p1, p2, p3)        // double, double, double
#define __mwerks_fnmadd(p1, p2, p3)       /* double */ __fnmadd(p1, p2, p3)       // double, double, double
#define __mwerks_fnmsub(p1, p2, p3)       /* double */ __fnmsub(p1, p2, p3)       // double, double, double
#define __mwerks_fsel(A, C, B)            /* double */ __fsel(A, C, B)            // double, double, double
#define __mwerks_fmadds(p1, p2, p3)       /* float  */ __fmadds(p1, p2, p3)       // float, float, float
#define __mwerks_fmsubs(p1, p2, p3)       /* float  */ __fmsubs(p1, p2, p3)       // float, float, float
#define __mwerks_fnmadds(p1, p2, p3)      /* float  */ __fnmadds(p1, p2, p3)      // float, float, float
#define __mwerks_fnmsubs(p1, p2, p3)      /* float  */ __fnmsubs(p1, p2, p3)      // float, float, float
#define __mwerks_fres(value)              /* float  */ __fres(value)              // float
#define __mwerks_fabsf(value)             /* float  */ __fabsf(value)             // float
#define __mwerks_fnabsf(value)            /* float  */ __fnabsf(value)            // float
#define __mwerks_frsqrte(value)           /* double */ __frsqrte(value)           // double
#define __mwerks_mffs()                   /* double */ __mffs()                   //
#define __mwerks_setflm(value)            /* float  */ __setflm(value)            // float
#define __mwerks_rlwinm(S, SH, MB, ME)    /* int    */ __rlwinm(S, SH, MB, ME)    // int, int, int, int
#define __mwerks_rlwnm(S, SH, MB, ME)     /* int    */ __rlwnm(S, SH, MB, ME)     // int, int, int, int
#define __mwerks_rlwimi(A, S, SH, MB, ME) /* int    */ __rlwimi(A, S, SH, MB, ME) // int, int, int, int, int
#define __mwerks_strcpy(dest, src)        /* char*  */ __strcpy(dest, src)        // char*, const char*
#define __mwerks_memcpy(dest, src, size)  /* void*  */ __memcpy(dest, src, size)  // void*, const void*, size_t
#define __mwerks_alloca(size)             /* void*  */ __alloca(size)             // unsigned long
#define __mwerks_va_setup(args)           /* void   */ __va_setup(args)           // void*
#define __mwerks_builtin_va_info(args)    /* void   */ __builtin_va_info(args)    // va_list*
#elif defined(__GNUC__)
// Portable equivalents for GCC/Clang builds (PS3 port). Semantics follow the Gekko instructions.
// Cache-control hints are no-ops; dcbz keeps its GameCube meaning (zero one 32-byte block).
#if defined(__powerpc__) || defined(__PPC__)
#define __mwerks_eieio() __asm__ volatile("eieio" ::: "memory")
#define __mwerks_sync()  __asm__ volatile("sync" ::: "memory")
#define __mwerks_isync() __asm__ volatile("isync" ::: "memory")
#else
#define __mwerks_eieio() __sync_synchronize()
#define __mwerks_sync()  __sync_synchronize()
#define __mwerks_isync() __sync_synchronize()
#endif
#define __mwerks_abs(value)               __builtin_abs(value)
#define __mwerks_labs(value)              __builtin_labs(value)
#define __mwerks_fabs(value)              __builtin_fabs(value)
#define __mwerks_fnabs(value)             (-__builtin_fabs(value))
#define __mwerks_cntlzw(value)            __ps3_cntlzw((u32)(value))
#define __mwerks_lhbrx(base, idx)         ((int)__builtin_bswap16(*(const u16*)((const u8*)(base) + (idx))))
#define __mwerks_lwbrx(base, idx)         ((int)__builtin_bswap32(*(const u32*)((const u8*)(base) + (idx))))
#define __mwerks_sthbrx(value, base, idx) (void)(*(u16*)((u8*)(base) + (idx)) = __builtin_bswap16((u16)(value)))
#define __mwerks_stwbrx(value, base, idx) (void)(*(u32*)((u8*)(base) + (idx)) = __builtin_bswap32((u32)(value)))
#define __mwerks_dcbf(base, offset)       ((void)(base), (void)(offset))
#define __mwerks_dcbt(base, offset)       ((void)(base), (void)(offset))
#define __mwerks_dcbst(base, offset)      ((void)(base), (void)(offset))
#define __mwerks_dcbtst(base, offset)     ((void)(base), (void)(offset))
#define __mwerks_dcbz(base, offset)       ((void)__builtin_memset((void*)(((__UINTPTR_TYPE__)(base) + (offset)) & ~(__UINTPTR_TYPE__)31), 0, 32))
#define __mwerks_mulhw(lhs, rhs)          ((int)(((s64)(s32)(lhs) * (s64)(s32)(rhs)) >> 32))
#define __mwerks_mulhwu(lhs, rhs)         ((u32)(((u64)(u32)(lhs) * (u64)(u32)(rhs)) >> 32))
#define __mwerks_divw(dividend, divisor)  ((int)(s32)(dividend) / (s32)(divisor))
#define __mwerks_divwu(dividend, divisor) ((u32)(dividend) / (u32)(divisor))
#define __mwerks_fmadd(p1, p2, p3)        ((double)(p1) * (double)(p2) + (double)(p3))
#define __mwerks_fmsub(p1, p2, p3)        ((double)(p1) * (double)(p2) - (double)(p3))
#define __mwerks_fnmadd(p1, p2, p3)       (-((double)(p1) * (double)(p2) + (double)(p3)))
#define __mwerks_fnmsub(p1, p2, p3)       (-((double)(p1) * (double)(p2) - (double)(p3)))
#define __mwerks_fsel(A, C, B)            (((double)(A) >= 0.0) ? (double)(C) : (double)(B))
#define __mwerks_fmadds(p1, p2, p3)       ((float)(p1) * (float)(p2) + (float)(p3))
#define __mwerks_fmsubs(p1, p2, p3)       ((float)(p1) * (float)(p2) - (float)(p3))
#define __mwerks_fnmadds(p1, p2, p3)      (-((float)(p1) * (float)(p2) + (float)(p3)))
#define __mwerks_fnmsubs(p1, p2, p3)      (-((float)(p1) * (float)(p2) - (float)(p3)))
#define __mwerks_fres(value)              (1.0f / (float)(value))
#define __mwerks_fabsf(value)             __builtin_fabsf(value)
#define __mwerks_fnabsf(value)            (-__builtin_fabsf(value))
#define __mwerks_frsqrte(value)           (1.0 / __builtin_sqrt((double)(value))) // exact; Gekko's estimate differs slightly
#define __mwerks_mffs()                   (0.0)                                   // FPSCR access not ported
#define __mwerks_setflm(value)            ((void)(value), 0.0f)                   // FPSCR access not ported
#define __mwerks_rlwinm(S, SH, MB, ME)    ((int)(__ps3_rotl32((u32)(S), (SH)) & __ps3_ppcmask((MB), (ME))))
#define __mwerks_rlwnm(S, SH, MB, ME)     ((int)(__ps3_rotl32((u32)(S), (SH)) & __ps3_ppcmask((MB), (ME))))
#define __mwerks_rlwimi(A, S, SH, MB, ME) ((int)(((u32)(A) & ~__ps3_ppcmask((MB), (ME))) | (__ps3_rotl32((u32)(S), (SH)) & __ps3_ppcmask((MB), (ME)))))
#define __mwerks_strcpy(dest, src)        __builtin_strcpy(dest, src)
#define __mwerks_memcpy(dest, src, size)  __builtin_memcpy(dest, src, size)
#define __mwerks_alloca(size)             __builtin_alloca(size)

static inline int __ps3_cntlzw(u32 x) { return x ? __builtin_clz(x) : 32; }
static inline u32 __ps3_rotl32(u32 x, unsigned int n) { n &= 31; return n ? (x << n) | (x >> (32 - n)) : x; }
// PowerPC mask: bits MB..ME set, numbered 0 = most significant bit, wrapping when MB > ME.
static inline u32 __ps3_ppcmask(unsigned int mb, unsigned int me)
{
	u32 m1 = 0xFFFFFFFFu >> (mb & 31);
	u32 m2 = 0xFFFFFFFFu << (31 - (me & 31));
	return ((mb & 31) <= (me & 31)) ? (m1 & m2) : (m1 | m2);
}

// Raw CodeWarrior intrinsics used directly by the MSL headers (include/stl/math.h).
static inline double __fabs(double x) { return __builtin_fabs(x); }
static inline float __fabsf(float x) { return __builtin_fabsf(x); }
static inline double __frsqrte(double x) { return 1.0 / __builtin_sqrt(x); } // exact; the NR steps in sqrtf then converge immediately
#endif

// Disable MSVC errors.  This is typically done via a compiler flag `/wdXXXX`, but that doesn't appear to work under MSVC 6.0.
#if defined(_MSC_VER)
#pragma warning(disable : 4716) // Allow non-void functions to return nothing.
#endif

#endif // _TYPES_H
