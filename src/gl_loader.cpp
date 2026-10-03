// SPDX-License-Identifier: MIT

#include "gl_loader.h"

#ifdef _WIN32

#include <atomic>
#include <mutex>

#define RAV_GL_DEF(ret, name, args) PFN_##name rav_##name = nullptr;
RAV_GL_FUNCS(RAV_GL_DEF)
#undef RAV_GL_DEF

namespace rav {
namespace {

// One resolved copy of the table (the field names go through the same #define
// routing as the globals, which is harmless inside a struct).
struct GlTable {
#define RAV_GL_FIELD(ret, name, args) PFN_##name name = nullptr;
    RAV_GL_FUNCS(RAV_GL_FIELD)
#undef RAV_GL_FIELD
};

std::mutex            g_gl_mutex;
bool                  g_gl_loaded = false;      // guarded by g_gl_mutex
std::atomic<unsigned> g_gl_generation{0};

// Resolves the whole table against the CURRENT context. False + the first missing
// name on failure. wglGetProcAddress only returns valid pointers while a context is
// current; the caller guarantees that.
bool Resolve(GlTable& t, std::string& out_error)
{
#define RAV_GL_LOAD(ret, name, args)                                          \
    t.name = reinterpret_cast<PFN_##name>(wglGetProcAddress(#name));          \
    if (!t.name) { out_error = "missing GL function: " #name; return false; }
    RAV_GL_FUNCS(RAV_GL_LOAD)
#undef RAV_GL_LOAD
    return true;
}

// Caller holds g_gl_mutex.
bool SameAsLoaded(const GlTable& t)
{
#define RAV_GL_SAME(ret, name, args) if (rav_##name != t.name) return false;
    RAV_GL_FUNCS(RAV_GL_SAME)
#undef RAV_GL_SAME
    return true;
}

// Caller holds g_gl_mutex.
void Commit(const GlTable& t)
{
#define RAV_GL_COMMIT(ret, name, args) rav_##name = t.name;
    RAV_GL_FUNCS(RAV_GL_COMMIT)
#undef RAV_GL_COMMIT
    g_gl_loaded = true;
    g_gl_generation.fetch_add(1);
}

}  // namespace

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
    // The first unresolved symbol aborts and names itself, so a capability gap is
    // diagnosable from the console.
    GlTable t;
    if (!Resolve(t, out_error)) return false;
    std::lock_guard<std::mutex> lock(g_gl_mutex);
    // Same pointers as already loaded (the normal case): write nothing, so a video
    // thread drawing at this moment never sees the table change under it.
    if (!g_gl_loaded || !SameAsLoaded(t)) Commit(t);
    return true;
}

bool CheckGlFunctionsForCurrentContext(std::string& out_error, unsigned* out_generation)
{
    GlTable t;
    if (!Resolve(t, out_error)) return false;
    std::lock_guard<std::mutex> lock(g_gl_mutex);
    if (!g_gl_loaded) {
        Commit(t);
    } else if (!SameAsLoaded(t)) {
        out_error = "this context's GL entry points differ from the viewer's";
        return false;
    }
    if (out_generation) *out_generation = g_gl_generation.load();
    return true;
}

unsigned GlFunctionsGeneration()
{
    return g_gl_generation.load();
}

}  // namespace rav

#endif  // _WIN32
