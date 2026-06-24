// SPDX-License-Identifier: MIT

#include "gl_loader.h"

#ifdef _WIN32

#define RAV_GL_DEF(ret, name, args) PFN_##name rav_##name = nullptr;
RAV_GL_FUNCS(RAV_GL_DEF)
#undef RAV_GL_DEF

namespace rav {

bool LoadGlFunctions(std::string& out_error)
{
#ifdef RAV_FORCE_INIT_FAILURE
    // TEST-ONLY (default OFF — never #defined in a normal build, so this block
    // vanishes and costs nothing: no runtime branch, no /W3 warning). Simulates an
    // old/unsupported driver so Story 1.4's graceful init-failure path (readable
    // diagnostic + symmetric teardown-on-bail + no host crash, AC1–AC3) can be
    // exercised on working hardware, where it never triggers naturally. Failing
    // HERE (rather than at context creation) also drives the new GL-version probe
    // in StartRendering. Flip on with: -DCMAKE_CXX_FLAGS="/D RAV_FORCE_INIT_FAILURE".
    out_error = "missing GL function: (forced by RAV_FORCE_INIT_FAILURE test build)";
    return false;
#endif
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
