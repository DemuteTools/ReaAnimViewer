// SPDX-License-Identifier: MIT
// See gltf_skin.h for why this exists (assimp 6.0.5 Windows drops >4-influence weights).

#ifdef _WIN32

#include "gltf_skin.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>

// rapidjson ships inside assimp's contrib tree; reuse it (no new dependency). Added as
// a SYSTEM include in CMake so its headers cannot trip our /W3 /permissive-.
#include <rapidjson/document.h>

#include <glm/gtc/type_ptr.hpp>  // glm::make_mat4 for the column-major inverse-bind read

namespace rav {
namespace {

namespace rj = rapidjson;

// ---- raw byte helpers -------------------------------------------------------------

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out)
{
    // Narrow path, matching how assimp itself opens the file (its DefaultIOSystem uses
    // narrow fopen on Windows) — so if assimp could read it, so can we.
    std::ifstream f(path.c_str(), std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff n = f.tellg();
    if (n <= 0) return false;
    out.resize(static_cast<size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), n);
    return static_cast<bool>(f);
}

// Minimal RFC 4648 base64 decoder for glTF "data:...;base64," buffer URIs.
bool Base64Decode(const std::string& in, std::vector<uint8_t>& out)
{
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    int bits = 0, acc = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const int v = val(c);
        if (v < 0) return false;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF)); }
    }
    return true;
}

// A resolved glTF buffer — a contiguous byte span the accessors index into.
struct Buffer { std::vector<uint8_t> bytes; };

// Component-type sizes / decoders (glTF §3.6.2.2).
size_t CompSize(int ct)
{
    switch (ct) {
        case 5120: case 5121: return 1;  // (U)BYTE
        case 5122: case 5123: return 2;  // (U)SHORT
        case 5125: case 5126: return 4;  // UINT / FLOAT
        default: return 0;
    }
}
int NumComps(const char* type)
{
    if (!type) return 0;
    if (!std::strcmp(type, "SCALAR")) return 1;
    if (!std::strcmp(type, "VEC2"))   return 2;
    if (!std::strcmp(type, "VEC3"))   return 3;
    if (!std::strcmp(type, "VEC4"))   return 4;
    if (!std::strcmp(type, "MAT4"))   return 16;
    return 0;
}
// Read one component as an unsigned integer (joint indices are U(BYTE|SHORT|INT)).
uint32_t ReadUint(const uint8_t* p, int ct)
{
    switch (ct) {
        case 5121: return *p;
        case 5123: { uint16_t v; std::memcpy(&v, p, 2); return v; }
        case 5125: { uint32_t v; std::memcpy(&v, p, 4); return v; }
        default:   return 0;
    }
}
// Read one component as a float (weights are FLOAT or normalized U(BYTE|SHORT)).
float ReadFloat(const uint8_t* p, int ct, bool normalized)
{
    switch (ct) {
        case 5126: { float v; std::memcpy(&v, p, 4); return v; }
        case 5121: return normalized ? (*p / 255.0f) : static_cast<float>(*p);
        case 5123: { uint16_t v; std::memcpy(&v, p, 2);
                     return normalized ? (v / 65535.0f) : static_cast<float>(v); }
        default:   return 0.0f;
    }
}

// Everything the code below needs about one accessor, already bounds-checked.
struct AccessorView {
    const uint8_t* base = nullptr;  // first element
    size_t stride = 0;              // bytes between elements
    int    compType = 0;
    int    numComps = 0;
    size_t count = 0;
    bool   normalized = false;
    bool   ok = false;
};

const rj::Value* Member(const rj::Value& v, const char* k)
{
    if (!v.IsObject()) return nullptr;
    auto it = v.FindMember(k);
    return it != v.MemberEnd() ? &it->value : nullptr;
}
int IntMember(const rj::Value& v, const char* k, int dflt)
{
    const rj::Value* m = Member(v, k);
    return (m && m->IsInt()) ? m->GetInt() : dflt;
}

// Resolves accessor #ai against the parsed doc + resolved buffers. Returns ok=false on
// any missing field, unknown type, or an out-of-range span (defensive — AR18).
AccessorView ResolveAccessor(const rj::Document& d, const std::vector<Buffer>& buffers,
                             int ai)
{
    AccessorView av;
    const rj::Value* accs = Member(d, "accessors");
    const rj::Value* bvs  = Member(d, "bufferViews");
    if (!accs || !accs->IsArray() || ai < 0 ||
        static_cast<size_t>(ai) >= accs->Size()) return av;
    const rj::Value& acc = (*accs)[ai];
    const rj::Value* typeM = Member(acc, "type");
    av.compType = IntMember(acc, "componentType", 0);
    av.numComps = typeM && typeM->IsString() ? NumComps(typeM->GetString()) : 0;
    av.count    = static_cast<size_t>(IntMember(acc, "count", 0));
    const rj::Value* nrm = Member(acc, "normalized");
    av.normalized = nrm && nrm->IsBool() && nrm->GetBool();
    const size_t cs = CompSize(av.compType);
    if (!cs || !av.numComps || !av.count) return av;
    // ReadUint/ReadFloat only decode unsigned-int + float component types. Signed
    // BYTE/SHORT (5120/5122, used only by KHR_mesh_quantization) would otherwise pass
    // CompSize yet read as all-zeros and silently *replace* assimp's correct geometry.
    // Reject them (ok stays false) so the caller falls back to assimp.
    if (av.compType == 5120 || av.compType == 5122) return av;
    // Sparse accessors substitute a subset of values from a second buffer view; we do
    // not apply that substitution, so reading the base view would yield wrong data that
    // overrides assimp's. Bail to the assimp fallback instead.
    if (Member(acc, "sparse")) return av;

    const int bvi = IntMember(acc, "bufferView", -1);
    if (!bvs || !bvs->IsArray() || bvi < 0 ||
        static_cast<size_t>(bvi) >= bvs->Size()) return av;
    const rj::Value& bv = (*bvs)[bvi];
    const int buf = IntMember(bv, "buffer", -1);
    if (buf < 0 || static_cast<size_t>(buf) >= buffers.size()) return av;
    const std::vector<uint8_t>& bytes = buffers[buf].bytes;

    const size_t bvOff = static_cast<size_t>(IntMember(bv, "byteOffset", 0));
    const size_t bvLen = static_cast<size_t>(IntMember(bv, "byteLength", 0));
    const size_t accOff = static_cast<size_t>(IntMember(acc, "byteOffset", 0));
    const size_t elem = cs * static_cast<size_t>(av.numComps);
    size_t stride = static_cast<size_t>(IntMember(bv, "byteStride", 0));
    if (stride == 0) stride = elem;
    if (stride < elem) return av;

    const size_t start = bvOff + accOff;
    // Last byte touched: start + (count-1)*stride + elem.
    if (av.count == 0) return av;
    const size_t last = start + (av.count - 1) * stride + elem;
    if (last > bytes.size()) return av;
    if (bvLen && start + (av.count - 1) * stride + elem > bvOff + bvLen &&
        // some exporters under-report byteLength; only reject if it also overruns buffer
        last > bytes.size()) return av;

    av.base = bytes.data() + start;
    av.stride = stride;
    av.ok = true;
    return av;
}

}  // namespace

GltfSkinData LoadGltfSkin(const std::string& path)
{
    GltfSkinData result;

    std::vector<uint8_t> file;
    if (!ReadFileBytes(path, file) || file.size() < 12) return result;

    // ---- split JSON + the embedded/first buffer ----
    std::string json;
    std::vector<uint8_t> glbBin;      // .glb BIN chunk (buffer 0), if any
    bool haveGlbBin = false;

    const bool isGlb = std::memcmp(file.data(), "glTF", 4) == 0;
    if (isGlb) {
        uint32_t total = 0; std::memcpy(&total, file.data() + 8, 4);
        size_t off = 12;
        while (off + 8 <= file.size()) {
            uint32_t clen = 0, ctype = 0;
            std::memcpy(&clen, file.data() + off, 4);
            std::memcpy(&ctype, file.data() + off + 4, 4);
            off += 8;
            if (off + clen > file.size()) return result;
            if (ctype == 0x4E4F534Au)            // "JSON"
                json.assign(reinterpret_cast<const char*>(file.data() + off), clen);
            else if (ctype == 0x004E4942u) {      // "BIN\0"
                glbBin.assign(file.begin() + off, file.begin() + off + clen);
                haveGlbBin = true;
            }
            off += clen + (clen % 4 ? 4 - clen % 4 : 0);  // chunks are 4-byte aligned
            (void)total;
        }
        if (json.empty()) return result;
    } else {
        json.assign(reinterpret_cast<const char*>(file.data()), file.size());
    }

    rj::Document d;
    d.Parse(json.c_str());
    if (d.HasParseError() || !d.IsObject()) return result;

    // ---- resolve every buffer (glb BIN, external file, or base64 data URI) ----
    std::vector<Buffer> buffers;
    if (const rj::Value* bufs = Member(d, "buffers"); bufs && bufs->IsArray()) {
        // Directory of the .gltf, for resolving external buffer files (string-only, no
        // std::filesystem — keeps this narrow-path and header-portable).
        const size_t slash = path.find_last_of("/\\");
        const std::string base = slash == std::string::npos ? std::string()
                                                            : path.substr(0, slash + 1);
        for (rj::SizeType i = 0; i < bufs->Size(); ++i) {
            Buffer b;
            const rj::Value& jb = (*bufs)[i];
            const rj::Value* uri = Member(jb, "uri");
            if (!uri || !uri->IsString()) {
                if (i == 0 && haveGlbBin) b.bytes = glbBin;  // glb: buffer 0 == BIN chunk
            } else {
                const std::string u = uri->GetString();
                const std::string tag = "base64,";
                const size_t p = u.find(tag);
                if (u.rfind("data:", 0) == 0 && p != std::string::npos) {
                    if (!Base64Decode(u.substr(p + tag.size()), b.bytes)) b.bytes.clear();
                } else {
                    // external file, URI relative to the .gltf (no percent-decoding — our
                    // tool-produced files use plain names)
                    ReadFileBytes(base + u, b.bytes);
                }
            }
            buffers.push_back(std::move(b));
        }
    }
    if (buffers.empty()) return result;

    // ---- skin joint names (indexed by glTF joint-array index) ----
    const rj::Value* skins = Member(d, "skins");
    const rj::Value* nodes = Member(d, "nodes");
    if (!skins || !skins->IsArray() || skins->Empty() || !nodes || !nodes->IsArray())
        return result;
    const rj::Value* joints = Member((*skins)[0], "joints");
    if (!joints || !joints->IsArray()) return result;
    result.jointNames.reserve(joints->Size());
    for (rj::SizeType i = 0; i < joints->Size(); ++i) {
        std::string name;
        if ((*joints)[i].IsInt()) {
            const int ni = (*joints)[i].GetInt();
            if (ni >= 0 && static_cast<rj::SizeType>(ni) < nodes->Size())
                if (const rj::Value* nm = Member((*nodes)[ni], "name"); nm && nm->IsString())
                    name = nm->GetString();
        }
        result.jointNames.push_back(std::move(name));
    }

    // ---- inverse-bind matrices (skin.inverseBindMatrices; optional) ----
    // glTF stores them column-major, exactly like glm — read 16 floats straight in, no
    // transpose. assimp's Windows build corrupts a few of these, spraying the affected
    // bone's verts into needle-spikes; overriding with the file's values fixes them.
    if (const rj::Value* ibmA = Member((*skins)[0], "inverseBindMatrices");
        ibmA && ibmA->IsInt()) {
        AccessorView iv = ResolveAccessor(d, buffers, ibmA->GetInt());
        if (iv.ok && iv.numComps == 16 && iv.compType == 5126) {
            result.inverseBind.resize(iv.count);
            for (size_t i = 0; i < iv.count; ++i) {
                float f[16];
                const uint8_t* p = iv.base + i * iv.stride;
                std::memcpy(f, p, sizeof(f));
                result.inverseBind[i] = glm::make_mat4(f);
            }
        }
    }

    // ---- per-primitive top-4 influences ----
    const rj::Value* meshes = Member(d, "meshes");
    if (!meshes || !meshes->IsArray()) return result;
    for (rj::SizeType mi = 0; mi < meshes->Size(); ++mi) {
        const rj::Value* prims = Member((*meshes)[mi], "primitives");
        if (!prims || !prims->IsArray()) continue;
        for (rj::SizeType pi = 0; pi < prims->Size(); ++pi) {
            const rj::Value* attrs = Member((*prims)[pi], "attributes");
            if (!attrs || !attrs->IsObject()) continue;
            const rj::Value* jA = Member(*attrs, "JOINTS_0");
            const rj::Value* wA = Member(*attrs, "WEIGHTS_0");
            const rj::Value* pA = Member(*attrs, "POSITION");
            if (!jA || !wA || !pA || !jA->IsInt() || !wA->IsInt() || !pA->IsInt())
                continue;  // not a skinned primitive

            AccessorView pos = ResolveAccessor(d, buffers, pA->GetInt());
            if (!pos.ok || pos.numComps < 3) continue;

            // Up to two influence sets (JOINTS_0/WEIGHTS_0 [+ _1]) = up to 8 bones.
            AccessorView jv[2], wv[2];
            jv[0] = ResolveAccessor(d, buffers, jA->GetInt());
            wv[0] = ResolveAccessor(d, buffers, wA->GetInt());
            if (!jv[0].ok || !wv[0].ok || jv[0].count != pos.count) continue;
            if (const rj::Value* j1 = Member(*attrs, "JOINTS_1"),
                               * w1 = Member(*attrs, "WEIGHTS_1");
                j1 && w1 && j1->IsInt() && w1->IsInt()) {
                jv[1] = ResolveAccessor(d, buffers, j1->GetInt());
                wv[1] = ResolveAccessor(d, buffers, w1->GetInt());
            }

            GltfMeshSkin ms;
            ms.vertexCount = pos.count;
            // Full mesh-local positions (assimp's Windows build also mis-reads a few of
            // these, throwing individual verts into thin needle-spikes).
            ms.positions.resize(pos.count);
            { const size_t cs = CompSize(pos.compType);
              for (size_t v = 0; v < pos.count; ++v) {
                  const uint8_t* p = pos.base + v * pos.stride;
                  ms.positions[v] = glm::vec3(ReadFloat(p, pos.compType, false),
                                              ReadFloat(p + cs, pos.compType, false),
                                              ReadFloat(p + 2 * cs, pos.compType, false));
              } }
            ms.firstPos = ms.positions[0];

            // Triangle indices (mode 4 / default). Non-indexed or non-triangle prims
            // leave `indices` empty → the caller keeps assimp's faces.
            const rj::Value* modeV = Member((*prims)[pi], "mode");
            const int mode = (modeV && modeV->IsInt()) ? modeV->GetInt() : 4;
            if (const rj::Value* iA = Member((*prims)[pi], "indices");
                iA && iA->IsInt() && mode == 4) {
                AccessorView iv = ResolveAccessor(d, buffers, iA->GetInt());
                if (iv.ok && iv.numComps == 1 && (iv.count % 3) == 0) {
                    ms.indices.resize(iv.count);
                    const size_t ics = CompSize(iv.compType);
                    for (size_t k = 0; k < iv.count; ++k)
                        ms.indices[k] = ReadUint(iv.base + k * iv.stride, iv.compType);
                }
            }

            ms.jointLocal.resize(pos.count, {0, 0, 0, 0});
            ms.weight.resize(pos.count, {0.0f, 0.0f, 0.0f, 0.0f});

            const size_t njoints = result.jointNames.size();
            for (size_t v = 0; v < pos.count; ++v) {
                // Gather all (<=8) influences, then keep the four heaviest.
                int   gid[8]; float gw[8]; int gn = 0;
                for (int set = 0; set < 2; ++set) {
                    if (!jv[set].ok || !wv[set].ok || v >= jv[set].count || v >= wv[set].count)
                        continue;
                    const size_t jcs = CompSize(jv[set].compType);
                    const size_t wcs = CompSize(wv[set].compType);
                    const uint8_t* jp = jv[set].base + v * jv[set].stride;
                    const uint8_t* wp = wv[set].base + v * wv[set].stride;
                    for (int k = 0; k < 4; ++k) {
                        const float w = ReadFloat(wp + k * wcs, wv[set].compType, wv[set].normalized);
                        if (!(w > 0.0f)) continue;
                        const uint32_t jl = ReadUint(jp + k * jcs, jv[set].compType);
                        if (jl >= njoints) continue;  // stray index -> drop this influence
                        gid[gn] = static_cast<int>(jl); gw[gn] = w; ++gn;
                        if (gn == 8) break;
                    }
                }
                // Selection-sort the top 4 (gn is tiny, no allocation).
                const int keep = gn < 4 ? gn : 4;
                for (int a = 0; a < keep; ++a) {
                    int best = a;
                    for (int b = a + 1; b < gn; ++b) if (gw[b] > gw[best]) best = b;
                    std::swap(gw[a], gw[best]); std::swap(gid[a], gid[best]);
                    ms.jointLocal[v][a] = gid[a];
                    ms.weight[v][a]     = gw[a];
                }
            }
            result.meshes.push_back(std::move(ms));
        }
    }

    result.ok = !result.meshes.empty();
    return result;
}

}  // namespace rav

#endif  // _WIN32
