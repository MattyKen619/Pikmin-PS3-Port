#ifndef _SYSNEW_H
#define _SYSNEW_H

#include "system.h"
#include "types.h"
#include <stddef.h>

#if defined(WIN32)
void* operator new(size_t size);
void* operator new[](size_t size);
#else
inline void* operator new(size_t size)
{
	return System::alloc(size);
}
inline void* operator new[](size_t size)
{
	return System::alloc(size);
}
#endif
void* operator new(size_t size, int alignment);
void* operator new[](size_t size, int alignment);
void operator delete(void* ptr);
void operator delete[](void* ptr);

// MetroWerks allowed programmers to take the address of an rvalue, but this is illegal
// in C and C++.  This illegal operation effectively acts like a placement new on stack
// allocated space; the only difference is that object destruction would be automatic.
// Pikmin 1 almost never uses destructors, however, so this should be of no consequence.
#if defined(__MWERKS__)
#define stack_new(...) &__VA_ARGS__
#elif defined(_MSC_VER) && _MSC_VER < 1400 // Visual Studio 2005 (8.0)
// MSVC also permits taking the address of an rvalue, however support for variadic macros
// was added after Visual Studio 6.0 (the version we must use for matching the DLL).  The
// reason we wish to use `__VA_ARGS__` here is to support templated types.  This includes
// for modding purposes, so we shouldn't to drop this macro's capabilities to that of the
// least capable compiler.  Luckily, we never NEED to pass a templated type name directly
// to this macro in any matching scenarios, because you can always typedef it to a simpler
// name and use that.
#define stack_new(type) &type
#elif defined(__cplusplus) && __cplusplus >= 201103L
// Legal C++11 equivalent (used by the PS3 port): `stack_new(T)(args...)` constructs T and yields a T*.
// The game keeps these pointers past the end of the expression (`TekiAndCondition cond(stack_new(...)
// (...), ...);` then `findClosest(pos, &cond)`), which with MetroWerks worked because the temporary's
// stack slot stayed put for the rest of the function. A temporary that lives only to the end of the
// expression does not: once the compiler reused its slot (test build 33, after the game's code was
// optimized more), a condition pointed at itself and the loading of the second area never finished. So the
// object goes into a ring of memory (ps3StackNewAlloc) that is reused only after much more has been
// allocated there, and like the original it is not destroyed (these types have nothing to destroy).
inline void* operator new(size_t, void* place) { return place; }
inline void operator delete(void*, void*) { }
extern "C" void* ps3StackNewAlloc(unsigned int size, unsigned int align);

template <typename T>
struct StackNewHolder {
	template <typename... Args>
	T* operator()(Args&&... args)
	{
		return new (ps3StackNewAlloc(sizeof(T), alignof(T))) T(static_cast<Args&&>(args)...);
	}
};
#define stack_new(...) StackNewHolder<__VA_ARGS__>()
#else
#define stack_new(...) &__VA_ARGS__ /* This must be replaced with something legal or be removed. */
#endif

#endif // _SYSNEW_H
