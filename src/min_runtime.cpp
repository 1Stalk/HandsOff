#include <windows.h>
#include <cstdlib>
#include <new>

#if defined(_HAS_EXCEPTIONS) && (_HAS_EXCEPTIONS == 0)

extern "C" int _fltused = 0;

void* __cdecl operator new(size_t size) {
    void* p = std::malloc(size ? size : 1);
    if (!p) __fastfail(7);
    return p;
}

void* __cdecl operator new[](size_t size) {
    return ::operator new(size);
}

void* __cdecl operator new(size_t size, const std::nothrow_t&) noexcept {
    return std::malloc(size ? size : 1);
}

void* __cdecl operator new[](size_t size, const std::nothrow_t&) noexcept {
    return std::malloc(size ? size : 1);
}

void __cdecl operator delete(void* p) noexcept {
    std::free(p);
}

void __cdecl operator delete(void* p, size_t) noexcept {
    std::free(p);
}

void __cdecl operator delete[](void* p) noexcept {
    std::free(p);
}

void __cdecl operator delete[](void* p, size_t) noexcept {
    std::free(p);
}

void __cdecl operator delete(void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}

void __cdecl operator delete[](void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}

namespace std {
[[noreturn]] void __cdecl _Xbad_alloc() { __fastfail(7); }
[[noreturn]] void __cdecl _Xlength_error(const char*) { __fastfail(7); }
[[noreturn]] void __cdecl _Xout_of_range(const char*) { __fastfail(7); }
} // namespace std

#endif
