// SPDX-License-Identifier: MIT

#include "gl_loader.h"

#ifdef _WIN32

#define RAV_GL_DEF(ret, name, args) PFN_##name rav_##name = nullptr;
RAV_GL_FUNCS(RAV_GL_DEF)
#undef RAV_GL_DEF

namespace rav {

bool LoadGlFunctions(std::string& out_error)
{
    // wglGetProcAddress only returns valid pointers while a context is current;
    // the caller guarantees that. The first unresolved symbol aborts and names
    // itself, so a capability gap is diagnosable from the console.
#define RAV_GL_LOAD(ret, name, args)                                          \
    rav_##name = reinterpret_cast<PFN_##name>(wglGetProcAddress(#name));      \
    if (!rav_##name) { out_error = "missing GL function: " #name; return false; }
    RAV_GL_FUNCS(RAV_GL_LOAD)
#undef RAV_GL_LOAD
    return true;
}

}  // namespace rav

#endif  // _WIN32
