// PS3 port: memory for the renderer, from the C library's malloc - never from the global operator new.
//
// The game replaces the global operator new (sysNew.h: it allocates from the game's own heaps, which do
// not exist before the game sets them up and are not for other threads) and operator delete (which does
// nothing). Anything the renderer (and the fragment program assembler) allocates must therefore go through
// these: containers with MallocAllocator, objects with create/destroy. (Test 22's first package used
// std::string in static tables: their constructors ran before main and asked the game for memory.)
//
// The build checks that the renderer's objects do not refer to operator new or delete (Makefile.ps3).

#ifndef PS3_RSX_ALLOC_H
#define PS3_RSX_ALLOC_H

#include <list>
#include <map>
#include <new>
#include <stack>
#include <deque>
#include <stddef.h>
#include <stdlib.h>
#include <string>
#include <vector>

namespace gfx {

template <class T>
struct MallocAllocator {
	typedef T value_type;
	MallocAllocator() { }
	template <class U>
	MallocAllocator(const MallocAllocator<U>&)
	{
	}
	T* allocate(size_t n)
	{
		const size_t bytes = n * sizeof(T);
		void* p            = malloc(bytes != 0 ? bytes : 1);
		if (!p) {
			abort();
		}
		return static_cast<T*>(p);
	}
	void deallocate(T* p, size_t) { free(p); }
	template <class U>
	bool operator==(const MallocAllocator<U>&) const
	{
		return true;
	}
	template <class U>
	bool operator!=(const MallocAllocator<U>&) const
	{
		return false;
	}
};

template <class T>
using Vec = std::vector<T, MallocAllocator<T> >;
template <class T>
using List = std::list<T, MallocAllocator<T> >;
template <class T>
using Stack = std::stack<T, std::deque<T, MallocAllocator<T> > >;
template <class K, class V, class C = std::less<K> >
using Map = std::map<K, V, C, MallocAllocator<std::pair<const K, V> > >;
typedef std::basic_string<char, std::char_traits<char>, MallocAllocator<char> > Str;

template <class T, class... Args>
T* create(Args&&... args)
{
	void* p = malloc(sizeof(T));
	if (!p) {
		abort();
	}
	return new (p) T(static_cast<Args&&>(args)...); // (placement new: no allocation)
}

template <class T>
void destroy(T* p)
{
	if (p) {
		p->~T();
		free(p);
	}
}

} // namespace gfx

#endif
