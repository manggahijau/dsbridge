/*
 * Minimal libstdc++.so replacement.
 *
 * Modern Android (roughly 9+) no longer ships /system/lib/libstdc++.so at
 * all (everything moved to libc++). Some old vendor blobs -- like this
 * Dolby DS1 libdseffect.so from ~2014-2015 -- were linked against the
 * classic GNU libstdc++ and simply record "libstdc++.so" as a NEEDED
 * dependency at link time. The dynamic linker requires a library
 * literally named libstdc++.so to exist and export the referenced
 * symbols; it does not matter that bionic's own libc.so already
 * implements equivalent logic under those exact same symbol names,
 * because ELF NEEDED resolution is by declared library name, not by
 * "any library that happens to export this symbol".
 *
 * This file does NOT reimplement a C++ standard library. It only
 * re-exports the small set of symbols this specific binary actually
 * references (confirmed via `readelf --dyn-syms`):
 *   - operator new / delete (regular and array forms)
 *   - __cxa_atexit / __cxa_finalize
 *   - __aeabi_unwind_cpp_pr0 / __aeabi_unwind_cpp_pr1 (ARM EHABI
 *     personality routines used by the C++ unwinder)
 *
 * Everything except operator new/delete is forwarded to bionic's own
 * real implementation (resolved via dlsym(RTLD_NEXT, ...) at first call)
 * rather than reimplemented, since bionic already has correct, real
 * versions of all of these -- we're only here to satisfy the "must come
 * from a library named libstdc++.so" requirement.
 */
#define _GNU_SOURCE
#include <cstdlib>
#include <dlfcn.h>

extern "C" {

typedef int (*cxa_atexit_fn)(void (*)(void*), void*, void*);
typedef void (*cxa_finalize_fn)(void*);
/* ARM EHABI personality routines follow normal AAPCS calling convention:
 * (state, exception-control-block ptr, unwind-context ptr) -> reason code.
 * Using generic int/pointer types here matches the real ABI layout even
 * without pulling in the full unwind-cpp.h type definitions. */
typedef int (*unwind_pr_fn)(int, void*, void*);

int __cxa_atexit(void (*destructor)(void*), void *arg, void *dso_handle) {
    static cxa_atexit_fn real = nullptr;
    if (!real) real = reinterpret_cast<cxa_atexit_fn>(dlsym(RTLD_NEXT, "__cxa_atexit"));
    return real ? real(destructor, arg, dso_handle) : 0;
}

void __cxa_finalize(void *dso_handle) {
    static cxa_finalize_fn real = nullptr;
    if (!real) real = reinterpret_cast<cxa_finalize_fn>(dlsym(RTLD_NEXT, "__cxa_finalize"));
    if (real) real(dso_handle);
}

int __aeabi_unwind_cpp_pr0(int state, void *ucbp, void *context) {
    static unwind_pr_fn real = nullptr;
    if (!real) real = reinterpret_cast<unwind_pr_fn>(dlsym(RTLD_NEXT, "__aeabi_unwind_cpp_pr0"));
    return real ? real(state, ucbp, context) : 0;
}

int __aeabi_unwind_cpp_pr1(int state, void *ucbp, void *context) {
    static unwind_pr_fn real = nullptr;
    if (!real) real = reinterpret_cast<unwind_pr_fn>(dlsym(RTLD_NEXT, "__aeabi_unwind_cpp_pr1"));
    return real ? real(state, ucbp, context) : 0;
}

} // extern "C"

void* operator new(std::size_t size) {
    return malloc(size ? size : 1);
}
void* operator new[](std::size_t size) {
    return malloc(size ? size : 1);
}
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
