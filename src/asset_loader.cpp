// SPDX-License-Identifier: MIT
//
// The ONE translation unit that includes <assimp/...> (D5). Every parse-exception
// risk is contained in the single try/catch in LoadAsset, so nothing throws across
// the subsystem boundary (AR18). This TU is also allowed to call modern GL: it owns
// the geometry AND texture upload, which is why LoadErrorCategory::GpuUploadFailed
// lives here (D6 — the loader reports its own GPU failures). See Dev Notes §E/§F.
//
// Story 2.2 adds two recorded boundary extensions, both in-character for this TU:
// it now also includes <stb_image.h> (assimp leaves embedded glTF textures as raw
// compressed bytes and exposes no decoder — see §C) and "console_log.h" (the loader
// is the only site that knows which texture failed, so the AR16 per-texture warning
// belongs here, exactly as the GPU-upload diagnostics already do).

#include "asset_loader.h"

#ifdef _WIN32

#include <algorithm>  // std::clamp for the shininess floor/ceiling
#include <cctype>     // std::tolower for the case-insensitive glTF extension check
#include <climits>   // INT_MAX for the encoded-texture size guard
#include <cmath>
#include <cstddef>
#include <cstring>    // std::strlen for the extension check
#include <filesystem>
#include <fstream>    // ParseFailureHint reads the file's first bytes (ASCII FBX check)
#include <iterator>   // istreambuf_iterator: external texture bytes (Story 11-2)
#include <string>
#include <system_error>
#include <unordered_map>  // bone name -> global skeleton index (D1 skin remap)
#include <unordered_set>
#include <vector>

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>  // glm::quat_cast for the bind-local TRS decompose (§C)

// stb_image: implementation compiled into THIS TU only. Narrowed to the formats
// glTF/Collada actually ship (mirrors the AR5 importer narrowing) to trim code and
// attack surface. We log our own reason strings, so the failure-string table is off.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_NO_FAILURE_STRINGS
// stbi_load() opens the sibling file by name; on Windows the default fopen uses the
// system codepage and would mangle a non-ASCII path. UTF8 makes stb convert our
// u8string() to wide + _wfopen, matching FileExists's u8path Unicode handling so a
// texture under an accented/CJK folder (gate row 9) resolves instead of warning.
#define STBI_WINDOWS_UTF8
#include <stb_image.h>

#include "animation.h"  // header-only pose sampler (ComputePose) for the §E audit probe
#include "console_log.h"  // LogWarn for the per-texture unresolved diagnostic (AR16)
#include "gl_loader.h"  // modern-GL upload entry points (glGenBuffers/glBufferData)
#include "gltf_skin.h"  // direct glTF skin-weight read (assimp 6.0.5 Windows bug work-around)
#include "texture_locate.h"  // TextureFileName: the by-name lookup beside the model (issue #1)

namespace rav {
namespace {

// The single assimp -> GLM conversion site (AR9/D3). aiMatrix4x4 is row-major;
// glm::mat4 is column-major — transpose exactly once, here and nowhere else.
glm::mat4 ConvertAssimpMatrix(const aiMatrix4x4& m)
{
    return glm::mat4(m.a1, m.b1, m.c1, m.d1,
                     m.a2, m.b2, m.c2, m.d2,
                     m.a3, m.b3, m.c3, m.d3,
                     m.a4, m.b4, m.c4, m.d4);  // column k built from assimp row k
}

// Element-wise near-equality for the shared-bone offset-divergence check (§B/§F).
bool MatNearlyEqual(const glm::mat4& a, const glm::mat4& b)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::fabs(a[c][r] - b[c][r]) > 1e-4f) return false;
    return true;
}

// DFS the node tree pre-order, indexing a node the first time it is seen as a skin
// joint. Pre-order makes a node land in `bones` before any of its descendants, so
// parentIdx < index holds for every non-root bone — the invariant D13's single
// forward global-matrix pass relies on. `nearest_joint` is the global index of the
// closest joint ancestor (-1 at the root); intermediate non-joint nodes pass it
// through unchanged, so parentIdx points at the nearest *joint*, not the raw parent
// node.
//
// `acc` folds the local transforms of the intermediate non-joint nodes skipped since
// the parent joint (§C). 3.1 handed those transforms to 3.2: a joint's bind-local =
// (skipped non-joints' product) * this node's local, so globalMat[i] stays correct
// without a graph walk. `acc` restarts at identity below every joint; for an all-joint
// chain (Mixamo) it is always identity and bind-local is just the node's own local.
// `bind_local` stays index-aligned with `skel.bones`: one entry pushed per indexed joint.
void DfsIndexJoints(const aiNode* node, int nearest_joint, const glm::mat4& acc,
                    const std::unordered_set<std::string>& joint_names,
                    std::unordered_map<std::string, int>& name_to_index,
                    SceneSkeleton& skel, std::vector<glm::mat4>& bind_local)
{
    const std::string nm = node->mName.C_Str();
    const glm::mat4 local = ConvertAssimpMatrix(node->mTransformation);
    const bool is_joint = joint_names.count(nm) != 0;

    int parent_for_children = nearest_joint;
    // A joint is a fold boundary: its children's accumulator restarts at identity. A
    // non-joint accumulates its local into the running product for the joints below it.
    const glm::mat4 acc_for_children = is_joint ? glm::mat4(1.0f) : acc * local;

    if (is_joint && !name_to_index.count(nm)) {
        const int gidx = static_cast<int>(skel.bones.size());
        name_to_index[nm] = gidx;
        SceneBone bone;
        bone.parentIdx = nearest_joint;
        bone.name = nm;  // as-authored UTF-8, no transliteration (FR5/AR13)
        skel.bones.push_back(std::move(bone));  // inverseBindMatrix filled below
        bind_local.push_back(acc * local);      // §C: fold skipped non-joint ancestors
        parent_for_children = gidx;
    }
    for (unsigned i = 0; i < node->mNumChildren; ++i)
        DfsIndexJoints(node->mChildren[i], parent_for_children, acc_for_children,
                       joint_names, name_to_index, skel, bind_local);
}

// Builds the flat SceneSkeleton + a bone-name -> global-index map (Task 1 / §B).
// The bones are every joint that is either skin-weighted OR animated — the dedup'd
// union of every mesh's aiBone names AND every animation channel's target node name
// (Story 3.4: UE twist-bone rigs animate parent joints that carry no skin weights) —
// ordered parent-before-child by the node-tree DFS above. The inverse-bind matrix is
// aiBone::mOffsetMatrix passed through the ONE ConvertAssimpMatrix boundary exactly
// once (AC2/D3/AR9). Returns an empty skeleton for a boneless (static) file (FR6).
SceneSkeleton BuildSkeleton(const aiScene* scene,
                            std::unordered_map<std::string, int>& name_to_index,
                            std::vector<glm::mat4>& bind_local)
{
    SceneSkeleton skel;

    // 1. Collect the skin-joint names across every mesh's bones (dedup by name).
    std::unordered_set<std::string> joint_names;
    for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* mesh = scene->mMeshes[mi];
        if (!mesh) continue;
        for (unsigned bi = 0; bi < mesh->mNumBones; ++bi)
            if (const aiBone* b = mesh->mBones[bi])  // null slot on a malformed file
                joint_names.insert(b->mName.C_Str());
    }
    // 1b. Also seed from every animation channel's target node (Story 3.4 fix). An
    //     animation drives NODES, and on UE-style twist-bone rigs many animated parent
    //     joints (root/thigh/upperarm/spine_05/neck) carry NO skin weights, so they
    //     never appear in mBones. Without this they'd be static-folded into their
    //     children (§C acc) and their channel dropped by ParseAnimations — torso-only
    //     motion + shard spikes. Union across ALL clips so the skeleton is complete
    //     regardless of which clip the MVP samples (clip[0]). Guard null aiAnimation*/
    //     aiNodeAnim* slots (AR18, same discipline as ParseAnimations).
    for (unsigned ai = 0; ai < scene->mNumAnimations; ++ai) {
        const aiAnimation* anim = scene->mAnimations[ai];
        if (!anim) continue;
        for (unsigned ci = 0; ci < anim->mNumChannels; ++ci)
            if (const aiNodeAnim* ch = anim->mChannels[ci])
                joint_names.insert(ch->mNodeName.C_Str());
    }
    if (joint_names.empty()) return skel;  // static path — empty skeleton (FR6)

    // 2. Assign global indices by DFS pre-order (parent-before-child, §B), capturing
    //    each joint's bind-local transform (acc starts identity at the root).
    DfsIndexJoints(scene->mRootNode, -1, glm::mat4(1.0f), joint_names, name_to_index,
                   skel, bind_local);

    // 3. Fill inverse-bind matrices from each bone's FIRST occurrence. A joint that
    //    is in mBones but absent from the node tree (malformed file) was missed by
    //    the DFS — append it as a root so its weights still resolve, and warn.
    std::unordered_set<std::string> bind_set;   // names whose matrix is already set
    std::unordered_set<std::string> diverged;   // warn-once guard (§F)
    for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* mesh = scene->mMeshes[mi];
        if (!mesh) continue;
        for (unsigned bi = 0; bi < mesh->mNumBones; ++bi) {
            const aiBone* b = mesh->mBones[bi];
            if (!b) continue;  // null bone slot: a raw deref here is an SEH access
                               // violation (UB), NOT catchable — never cross the host
                               // boundary on a malformed file (AR18/NFR-R1).
            const std::string nm = b->mName.C_Str();
            const glm::mat4 ibm = ConvertAssimpMatrix(b->mOffsetMatrix);
            auto it = name_to_index.find(nm);
            if (it == name_to_index.end()) {
                const int gidx = static_cast<int>(skel.bones.size());
                name_to_index[nm] = gidx;
                SceneBone bone;
                bone.parentIdx = -1;
                bone.name = nm;
                bone.inverseBindMatrix = ibm;
                skel.bones.push_back(std::move(bone));
                bind_set.insert(nm);
                LogWarn("skeleton: bone '%s' absent from node tree - appended as root",
                        nm.c_str());
                continue;
            }
            if (!bind_set.count(nm)) {
                skel.bones[it->second].inverseBindMatrix = ibm;
                bind_set.insert(nm);
            } else if (!diverged.count(nm) &&
                       !MatNearlyEqual(skel.bones[it->second].inverseBindMatrix, ibm)) {
                // Same bone, different offset across meshes — keep the first; a shared
                // palette can't honor both (per-mesh palettes are a 3.3 call, §F).
                diverged.insert(nm);
                LogWarn("skeleton: bone '%s' has divergent bind matrices across meshes"
                        " - keeping first", nm.c_str());
            }
        }
    }
    // Step 3 may append bones absent from the node tree (DFS never reached them, so
    // they have no bind-local). Pad bind_local to the final count with identity so it
    // stays index-aligned with skel.bones; those degenerate roots are already warned.
    bind_local.resize(skel.bones.size(), glm::mat4(1.0f));
    return skel;
}

// The validator hook (Task 3 / §E, AC4): on a skinned load, dump the parsed skeleton
// through the [RAV] info channel so the bone count / names / parent links can be
// diffed by eye against Blender's Outliner or FBX Review — the only way to audit
// AC1 before 3.3 draws the rig deformed. Non-ASCII names print as their raw UTF-8
// bytes (the FR5 check). Silent for a boneless file, keeping the Epic 2 console clean.
void DumpSkeleton(const SceneSkeleton& skel, size_t skinned_verts)
{
    LogInfo("skeleton: %zu bones, %zu skinned verts", skel.bones.size(), skinned_verts);
    for (size_t i = 0; i < skel.bones.size(); ++i) {
        const SceneBone& b = skel.bones[i];
        const bool has_parent =
            b.parentIdx >= 0 && b.parentIdx < static_cast<int>(skel.bones.size());
        // parentIdx < i must hold for every non-root bone (DFS pre-order invariant);
        // surface a violation rather than letting 3.2's forward pass read it as valid.
        if (has_parent && b.parentIdx >= static_cast<int>(i))
            LogWarn("skeleton: bone [%zu] '%s' has parent %d >= index (ordering broken)",
                    i, b.name.c_str(), b.parentIdx);
        LogInfo("  [%2zu] %-20s parent %3d (%s)", i, b.name.c_str(), b.parentIdx,
                has_parent ? skel.bones[b.parentIdx].name.c_str() : "root");
    }
}

// Decomposes a bind-local matrix into the (T, R, S) of the single default keyframe a
// bone gets when the clip does not animate it — or for the component a rotation-only
// channel omits (so T/S fall back to rest, the AC2 "rotation-only poses correctly"
// path). Lossy under shear/mirror, but rigs are translate+rotate+~uniform-scale so it
// is exact in practice. Returns false on a degenerate basis (near-zero column or
// negative determinant) so the caller can surface it (§C) rather than silently passing.
bool DecomposeTRS(const glm::mat4& m, glm::vec3& t, glm::quat& r, glm::vec3& s)
{
    t = glm::vec3(m[3]);                      // translation column
    const glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    s = glm::vec3(glm::length(c0), glm::length(c1), glm::length(c2));  // column magnitudes

    bool ok = true;
    glm::vec3 inv(1.0f);
    for (int k = 0; k < 3; ++k) {
        if (s[k] > 1e-8f) inv[k] = 1.0f / s[k];  // de-scale to a pure rotation basis
        else { inv[k] = 0.0f; ok = false; }      // collapsed axis — rotation undefined
    }
    const glm::mat3 basis(c0 * inv.x, c1 * inv.y, c2 * inv.z);
    if (glm::determinant(basis) < 0.0f) ok = false;  // mirrored bind — quat_cast is lossy
    r = glm::quat_cast(basis);
    return ok;
}

// Parses the FIRST animation clip (MVP — scene.h:81) into one SceneAnimation whose
// channels are sized to and indexed by bone global index (§B). Every bone is first
// defaulted from its bind-local so the sampler never branches on a missing channel;
// the clip's aiNodeAnim tracks then overlay the joints they name. `out_animated`
// (per-bone) and `out_tps` feed the §E audit. Returns {} for a clip-less/boneless file.
std::vector<SceneAnimation> ParseAnimations(
    const aiScene* scene, const SceneSkeleton& skeleton,
    const std::unordered_map<std::string, int>& name_to_index,
    const std::vector<glm::mat4>& bind_local,
    std::vector<bool>& out_animated, double& out_tps)
{
    out_animated.assign(skeleton.bones.size(), false);
    out_tps = 0.0;
    if (scene->mNumAnimations == 0 || skeleton.bones.empty()) return {};  // FR6 / static path
    if (scene->mNumAnimations > 1)
        LogInfo("animation: %u clips, using [0]", scene->mNumAnimations);  // MVP: clip 0

    const aiAnimation* a = scene->mAnimations[0];
    if (!a) return {};  // null slot guard — a raw deref is SEH/UB, not catchable (AR18)

    // Ticks -> seconds at the boundary. assimp's FBX and glTF importers report
    // different mTicksPerSecond, so never hard-code a rate; 25 is assimp's own fallback.
    // Guard finite-AND-positive, not just != 0: a negative rate (broken FBX) would
    // sign-flip every key time, leaving the stored tracks time-DESCENDING while the
    // sampler assumes ascending -> wrong bracket, silently wrong pose; NaN poisons every
    // time value (NaN != 0.0 is true, so the bare != 0 check let it through).
    double tps = a->mTicksPerSecond;
    if (!(tps > 0.0) || !std::isfinite(tps)) {
        if (tps != 0.0)  // an actual bad value, not assimp's legitimate "unset" 0
            LogWarn("animation: non-finite/negative ticks-per-second %.3f - using 25",
                    tps);
        tps = 25.0;
    }
    out_tps = tps;

    SceneAnimation out;
    // Clamp >= 0: a negative mDuration would make ComputePose's clamp(t, 0, duration)
    // a degenerate range, freezing every probe at the end and silently zeroing the
    // audit's maxDelta (a real clip would read as a frozen pose).
    out.duration = std::max(0.0f, static_cast<float>(a->mDuration / tps));
    out.channels.resize(skeleton.bones.size());

    // 1. Default every bone's channel from its bind-local rest pose (the unanimated
    //    fallback). A bone the clip never names keeps these single keys and poses at rest.
    for (size_t i = 0; i < skeleton.bones.size(); ++i) {
        glm::vec3 dt, ds;
        glm::quat dr;
        if (!DecomposeTRS(bind_local[i], dt, dr, ds))
            LogWarn("animation: bone '%s' bind-local is sheared/mirrored - default key approximated",
                    skeleton.bones[i].name.c_str());
        out.channels[i].translation = {{0.0f, dt}};
        out.channels[i].rotation    = {{0.0f, dr}};
        out.channels[i].scale       = {{0.0f, ds}};
    }

    // 2. Overlay the clip's animated tracks onto the joints they target.
    for (unsigned c = 0; c < a->mNumChannels; ++c) {
        const aiNodeAnim* ch = a->mChannels[c];
        if (!ch) continue;  // null channel slot guard (AR18)
        auto it = name_to_index.find(ch->mNodeName.C_Str());
        if (it == name_to_index.end()) {
            // A track on a helper/mesh node, not a skin joint — folding intermediate
            // animated nodes needs the node tree at sample time (3.3+, §F). Char clips
            // animate joints directly, so warn once and skip.
            LogWarn("animation: channel '%s' targets a non-joint node - skipped",
                    ch->mNodeName.C_Str());
            continue;
        }
        AnimChannel& dst = out.channels[it->second];
        bool has_keys = false;  // a name match with all-empty key arrays drives nothing
        // Component copy — a vec3 is not a matrix, no ConvertAssimpMatrix (§D).
        if (ch->mNumPositionKeys > 0) {
            std::vector<KeyframeT> keys;
            keys.reserve(ch->mNumPositionKeys);
            for (unsigned k = 0; k < ch->mNumPositionKeys; ++k) {
                const aiVectorKey& vk = ch->mPositionKeys[k];
                keys.push_back({static_cast<float>(vk.mTime / tps),
                                glm::vec3(vk.mValue.x, vk.mValue.y, vk.mValue.z)});
            }
            dst.translation = std::move(keys);
            has_keys = true;
        }
        // Quaternion: assimp is {w,x,y,z}; glm::quat ctor is w-first. Wrong order gives
        // a plausible-but-wrong rotation that only shows as garbage in 3.3 — the §D
        // AC2-analog discipline. No handedness flip (render as-authored, D3).
        if (ch->mNumRotationKeys > 0) {
            std::vector<KeyframeR> keys;
            keys.reserve(ch->mNumRotationKeys);
            for (unsigned k = 0; k < ch->mNumRotationKeys; ++k) {
                const aiQuatKey& qk = ch->mRotationKeys[k];
                keys.push_back({static_cast<float>(qk.mTime / tps),
                                glm::quat(qk.mValue.w, qk.mValue.x, qk.mValue.y, qk.mValue.z)});
            }
            dst.rotation = std::move(keys);
            has_keys = true;
        }
        if (ch->mNumScalingKeys > 0) {
            std::vector<KeyframeS> keys;
            keys.reserve(ch->mNumScalingKeys);
            for (unsigned k = 0; k < ch->mNumScalingKeys; ++k) {
                const aiVectorKey& sk = ch->mScalingKeys[k];
                keys.push_back({static_cast<float>(sk.mTime / tps),
                                glm::vec3(sk.mValue.x, sk.mValue.y, sk.mValue.z)});
            }
            dst.scale = std::move(keys);
            has_keys = true;
        }
        if (has_keys) out_animated[it->second] = true;
    }

    std::vector<SceneAnimation> result;
    result.push_back(std::move(out));
    return result;
}

// The §E audit probe (AC5): the click-based validator hook for the sampler. On an
// animated load it logs the clip metadata, then samples the pose at t=0, t=mid, t=end
// and reports the root's global translation (root-motion evidence, FR7), a whole-
// palette finite check (AC3 boundary validity), and the max bone-position delta from
// t=0 to mid (proves the pose actually varies with time). These three lines are what
// AC1–AC3 are eyeballed against at the gate. Load-time scratch alloc is fine (not hot).
void DumpAnimation(const SceneSkeleton& skel, const SceneAnimation& anim,
                   const std::vector<bool>& animated, double tps)
{
    size_t animated_count = 0;
    for (bool b : animated) if (b) ++animated_count;
    size_t total_keys = 0;
    for (const AnimChannel& ch : anim.channels)
        total_keys += ch.translation.size() + ch.rotation.size() + ch.scale.size();

    LogInfo("animation: 1 clip, dur %.3fs (tps %.1f), %zu/%zu bones animated, %zu keys",
            anim.duration, tps, animated_count, skel.bones.size(), total_keys);

    int root = -1;  // first parentless bone — its global translation is the root motion
    for (size_t i = 0; i < skel.bones.size(); ++i)
        if (skel.bones[i].parentIdx < 0) { root = static_cast<int>(i); break; }

    const size_t n = skel.bones.size();
    std::vector<glm::mat4> palette(n), global(n), global0(n);

    const float probes[3] = {0.0f, anim.duration * 0.5f, anim.duration};
    for (int p = 0; p < 3; ++p) {
        ComputePose(skel, anim, probes[p], palette, global);

        bool finite = true;
        for (const glm::mat4& m : palette) {
            const float* f = &m[0][0];
            for (int e = 0; e < 16 && finite; ++e)
                if (!std::isfinite(f[e])) finite = false;
            if (!finite) break;
        }
        const glm::vec3 rtx = (root >= 0) ? glm::vec3(global[root][3]) : glm::vec3(0.0f);

        if (p == 0) {
            global0 = global;  // snapshot for the t0->mid delta below
            LogInfo("  probe t=%.3f  root tx (%7.2f,%7.2f,%7.2f)  palette finite=%s",
                    probes[p], rtx.x, rtx.y, rtx.z, finite ? "yes" : "no");
        } else if (p == 1) {
            float max_delta = 0.0f;
            for (size_t i = 0; i < n; ++i)
                max_delta = std::max(max_delta,
                    glm::length(glm::vec3(global[i][3]) - glm::vec3(global0[i][3])));
            LogInfo("  probe t=%.3f  root tx (%7.2f,%7.2f,%7.2f)  palette finite=%s"
                    "  maxDelta(t0->mid)=%.2f",
                    probes[p], rtx.x, rtx.y, rtx.z, finite ? "yes" : "no", max_delta);
        } else {
            LogInfo("  probe t=%.3f  root tx (%7.2f,%7.2f,%7.2f)  palette finite=%s",
                    probes[p], rtx.x, rtx.y, rtx.z, finite ? "yes" : "no");
        }
    }
}

// Reads diffuse factor + derives a per-material Blinn-Phong specular into a
// SceneMaterial (textures: ResolveTexture + UploadCpuImage, Story 2.2/6.5.1/11-2). The
// specular derivation (Story 2.3) is what makes a matte dielectric and a polished
// metal show a different highlight. Defaults match a neutral mid-grey surface.
SceneMaterial ConvertMaterial(const aiMaterial* mat)
{
    SceneMaterial out;
    out.baseColorFactor = glm::vec3(0.8f);   // mid-grey if the file carries no diffuse
    out.specularColor   = glm::vec3(0.04f);  // dielectric F0 fallback
    out.shininess       = 32.0f;
    if (!mat) return out;   // synthetic fallback material (file had none) — all defaults

    aiColor3D c;
    if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, c) == AI_SUCCESS)
        out.baseColorFactor = glm::vec3(c.r, c.g, c.b);

    // assimp synthesizes SHININESS for every importer we ship: glTF metallic-roughness
    // from (1-roughness)^2*1000, glTF specular-glossiness from glossiness*1000, FBX/
    // Collada Phong from the authored exponent. A fully-rough glTF material yields 0,
    // so clamp to a small floor (a broad, present highlight) rather than snapping to 32.
    float s = 0.0f;
    if (mat->Get(AI_MATKEY_SHININESS, s) == AI_SUCCESS)
        // std::clamp(NaN,…) returns NaN, which would poison pow() in the shader;
        // gate on isfinite so a garbage exponent falls back to the safe default.
        out.shininess = std::isfinite(s) ? std::clamp(s, 2.0f, 1000.0f) : 32.0f;

    // Specular color — ALWAYS derived from metalness (Story 6.5.1 gate fix), NEVER from
    // an authored COLOR_SPECULAR. FBX/OBJ Phong exports (notably Mixamo — the documented
    // "Mixamo materials are too shiny" problem, and assimp's format-dependent shininess
    // scaling) carry a gray/white Phong specular that makes skin/cloth read shiny/plastic
    // and washes the lit faces into smeared highlight bands. AC3 wants a DIELECTRIC MATTE
    // by default, so we ignore the authored value and reflect physics instead: a
    // dielectric reflects a dim ~4% white highlight (F0=0.04); a metal reflects its own
    // base color — tint toward baseColor by metalness, which keeps a genuine metal glossy
    // and preserves the FR16 matte-vs-glossy distinction without trusting bad Phong data.
    float metallic = 0.0f;
    mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic);  // leaves 0 (dielectric) if absent
    // metalness is defined on [0,1]; clamp (and reject NaN) so the mix stays a convex
    // blend — an out-of-range factor would push specular past baseColor or negative, a
    // NaN would poison the fragment.
    metallic = std::isfinite(metallic) ? std::clamp(metallic, 0.0f, 1.0f) : 0.0f;
    out.specularColor =
        glm::vec3(0.04f) + (out.baseColorFactor - glm::vec3(0.04f)) * metallic;
    return out;
}

// Uploads a 4-channel RGBA buffer to a fresh GL texture and returns its owning
// GpuImage (empty on GL failure → flat fallback). PRECONDITION: current GL context.
// Mirrors UploadMesh's glGetError discipline, but a texture failure is NON-fatal:
// the geometry is still drawable, so we degrade to the flat baseColorFactor rather
// than aborting the whole load (a buffer failure stays fatal — see UploadMesh).
//
// internal_format selects the GPU's interpretation of the 8-bit bytes (Story 6.5.1):
// a COLOUR texture uploads GL_SRGB8_ALPHA8 so the GPU decodes sRGB→linear on every
// sample (correct lighting math), while a NORMAL map uploads GL_RGBA8 (linear) — its
// bytes are geometry, not colour, and must NOT be gamma-decoded. The pixel-transfer
// format/type stays GL_RGBA/GL_UNSIGNED_BYTE either way (stb forces 4 channels).
GpuImage UploadTexture(const unsigned char* rgba, int w, int h, GLint internal_format)
{
    GpuImage tex;
    glGenTextures(1, tex.addr());
    if (tex.get() == 0) return {};

    // Drain stale errors so the post-upload check observes only THIS upload.
    while (glGetError() != GL_NO_ERROR) {}

    glBindTexture(GL_TEXTURE_2D, tex.get());
    // RGBA rows are 4-byte aligned anyway, but set 1 defensively so a future RGB
    // path (odd row stride) would still upload correctly.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);  // glTF sampler default
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);

    bool gl_failed = false;
    while (glGetError() != GL_NO_ERROR) gl_failed = true;
    if (gl_failed) return {};  // free the partial texture (RAII) → flat fallback
    return tex;
}

// The ONE unified texture funnel (AR14): every packaging variant — GLB embedded
// (FR17), glTF sibling file (FR18), and later FBX embedded (FR19, Epic 6) — converges
// on GetEmbeddedTexture. Story 11-2: it is GL-free (the parse runs on any thread and is
// shared): it keeps the image's encoded bytes once stb has validated their header, and
// UploadCpuImage decodes + uploads. Returns an empty image when the material declares no
// texture of `type` (silent — the textureless flat path, AC5) or the declared texture
// cannot be resolved (one LogWarn, then flat fallback — AC3). The upload always decodes
// 4-channel RGBA so the GL upload format is uniform regardless of the source's channel
// count. mi names the material in the diagnostic; `kind` labels it ("diffuse"/"normal").
// not_found (optional): set to the texture's FILE NAME when it is an external file that
// could not be opened at all (neither at its stored path nor by name beside the model);
// left untouched otherwise (found, embedded, undecodable, or not declared).
CpuImage ResolveTexture(const aiScene* scene, const aiMaterial* mat, aiTextureType type,
                        const std::filesystem::path& model_dir, unsigned mi, const char* kind,
                        std::string* not_found = nullptr)
{
    CpuImage out;
    aiString tex_path;
    if (!mat || mat->GetTexture(type, 0, &tex_path) != AI_SUCCESS)
        return out;   // no texture of this type declared — flat path, silent (not a failure)

    // Keeps the encoded bytes when stb recognizes their header (size known, nothing
    // decoded yet); the pixels are decoded at upload. False = not an image stb can read.
    auto keep_encoded = [&out](std::vector<unsigned char>&& bytes) {
        int w = 0, h = 0, n = 0;
        if (bytes.empty() || bytes.size() > static_cast<size_t>(INT_MAX) ||
            !stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n) ||
            w <= 0 || h <= 0)
            return false;
        out.width   = w;
        out.height  = h;
        out.encoded = std::move(bytes);
        return true;
    };

    if (const aiTexture* t = scene->GetEmbeddedTexture(tex_path.C_Str())) {
        // Embedded: GLB '*N' index reference today, FBX GetEmbeddedTexture tomorrow —
        // the same branch absorbs both, which is the whole point of the unified seam.
        if (t->mHeight == 0) {
            // Compressed: pcData holds mWidth bytes of a PNG/JPG file — stb decodes it
            // at upload (UploadCpuImage).
            const unsigned char* bytes = reinterpret_cast<const unsigned char*>(t->pcData);
            if (!keep_encoded(std::vector<unsigned char>(bytes, bytes + t->mWidth))) {
                LogWarn("%s texture unresolved for material %u (embedded image undecodable)"
                        " - using flat color", kind, mi);
                return {};
            }
            return out;
        }
        // Uncompressed: mWidth*mHeight aiTexel, stored B,G,R,A by assimp — read by the
        // named members so the result is RGBA regardless of in-memory channel order.
        const size_t count = static_cast<size_t>(t->mWidth) * t->mHeight;
        out.rgba.resize(count * 4);
        for (size_t i = 0; i < count; ++i) {
            const aiTexel& texel = t->pcData[i];
            out.rgba[i * 4 + 0] = texel.r;
            out.rgba[i * 4 + 1] = texel.g;
            out.rgba[i * 4 + 2] = texel.b;
            out.rgba[i * 4 + 3] = texel.a;
        }
        out.width  = static_cast<int>(t->mWidth);
        out.height = static_cast<int>(t->mHeight);
        return out;
    }

    // External sibling file — resolve relative to the model dir, read its bytes (decoded
    // at upload). Path carried as UTF-8 via u8path (consistent with FileExists's Unicode
    // fix); a percent-encoded/absolute-URI edge case simply fails to open and takes the
    // diagnostic path below, the model still rendering in flat color.
    //
    // Issue #1: then by FILE NAME beside the model. Exporters store paths from their own
    // folder tree ("..\..\sourceimages\T.png"), still outside the model folder after
    // REAPER copied the .fbx into the project: with this second try, putting the texture
    // files next to the .fbx is enough, whatever the exporter.
    const std::string stored = tex_path.C_Str();
    bool opened = false;
    auto try_file = [&](const std::filesystem::path& file) {
        std::vector<unsigned char> bytes;
        {
            std::ifstream f(file, std::ios::binary);
            if (!f) return false;
            opened = true;
            bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        return keep_encoded(std::move(bytes));
    };
    for (const std::string& candidate : TextureCandidates(stored))
        if (try_file(model_dir / std::filesystem::u8path(candidate))) return out;
    LogWarn("%s texture unresolved for material %u (%s %s) - using flat color", kind, mi,
            stored.c_str(), opened ? "undecodable" : "missing");
    // Only a real file name can be located; a path without one counts as unreadable.
    const std::string name = TextureFileName(stored);
    if (!opened && not_found && !name.empty()) *not_found = name;
    return {};
}

// GL half of the texture funnel: uploads one texture to the CURRENT context, decoding it
// first when it is still encoded (the decoded pixels are freed right after the upload —
// Story 2.2 AC6 — so only one decoded texture exists at a time). internal_format is the
// GPU interpretation: GL_SRGB8_ALPHA8 for colour, GL_RGBA8 (linear) for data maps (Story
// 6.5.1). An empty image gives an empty handle (flat path). A decode or upload failure
// warns once and counts in `failed` (flat fallback).
GpuImage UploadCpuImage(const CpuImage& img, GLint internal_format, const char* kind,
                        unsigned mi, int& failed)
{
    if (img.empty()) return {};
    GpuImage tex;
    if (!img.rgba.empty()) {
        tex = UploadTexture(img.rgba.data(), img.width, img.height, internal_format);
    } else {
        int w = 0, h = 0, n = 0;
        unsigned char* px = stbi_load_from_memory(img.encoded.data(),
                                                  static_cast<int>(img.encoded.size()),
                                                  &w, &h, &n, 4);   // 4 = force RGBA
        if (!px) {
            ++failed;
            LogWarn("%s texture unresolved for material %u (image undecodable)"
                    " - using flat color", kind, mi);
            return {};
        }
        tex = UploadTexture(px, w, h, internal_format);
        stbi_image_free(px);   // CPU pixels freed immediately after upload (AC6)
    }
    if (tex.get() == 0) {
        ++failed;
        LogWarn("%s texture unresolved for material %u (GPU upload failed)"
                " - using flat color", kind, mi);
    }
    return tex;
}

// Normal-map resolution: same unified funnel, upload LINEAR (GL_RGBA8) because a
// normal map is geometry, not colour, and must never be sRGB-decoded (Story 6.5.1 AC4).
// An empty image (no map declared) leaves u_hasNormalMap=0 → the geometric normal is
// used unchanged.
//
// ONLY aiTextureType_NORMALS (a true tangent-space normal map). We deliberately do NOT
// fall back to aiTextureType_HEIGHT: in FBX/OBJ that slot is the legacy "bump" channel,
// which is frequently a GRAYSCALE HEIGHT map, not a tangent-space normal map. Sampling a
// grayscale height `g` as a normal (`rgb*2-1` → ~(2g-1,2g-1,2g-1)) makes the shaded
// normal track the height iso-lines → the "melted-wax / contour-banding" artifact seen
// on downloaded FBX characters. A model whose only relief map is under HEIGHT simply
// renders with its geometric normal (clean, just no micro-detail) until a post-MVP
// height→normal (Sobel) conversion is added — far better than corrupting the shading.
CpuImage ResolveNormalMap(const aiScene* scene, const aiMaterial* mat,
                          const std::filesystem::path& model_dir, unsigned mi,
                          std::string* not_found)
{
    if (!mat) return {};
    if (mat->GetTextureCount(aiTextureType_NORMALS) > 0)
        return ResolveTexture(scene, mat, aiTextureType_NORMALS, model_dir, mi, "normal",
                              not_found);
    return {};   // no true normal map declared — geometric normal, silent (not a failure)
}

// Per-mesh skin weights read straight from a glTF file (src/gltf_skin.cpp), already
// reduced to the top-4 influences and remapped to GLOBAL bone indices. Populated ONLY
// for .glb/.gltf, to side-step an assimp 6.0.5 bug that (on the Windows build) corrupts
// the weights of >4-influence glTF meshes. `valid` is false for FBX/Collada and for any
// mesh we could not confidently match, in which case AppendMesh uses assimp's weights.
struct ResolvedMeshSkin {
    bool                    valid = false;
    std::vector<glm::ivec4> ids;       // per local vertex — global bone ids (top-4)
    std::vector<glm::vec4>  weights;   // per local vertex — weights (top-4, unnormalized)
    std::vector<glm::vec3>  positions; // per local vertex — mesh-local, straight from file
    std::vector<uint32_t>   indices;   // triangle list (empty → keep assimp faces)
};

// Appends one aiMesh, baked into model space by `world`, as a SceneMesh's CPU
// arrays. Vertices/indices go into the out params; the AABB is grown over the
// baked positions. Normals use the inverse-transpose so non-uniform node scale
// stays correct (matches the renderer's u_normal convention). When `gltf_skin` is
// valid its weights REPLACE assimp's for this mesh (the glTF-corruption work-around).
void AppendMesh(const aiMesh* mesh, const glm::mat4& world,
                const std::unordered_map<std::string, int>& bone_index,
                const ResolvedMeshSkin* gltf_skin,
                std::vector<SceneVertex>& verts, std::vector<uint32_t>& indices,
                glm::vec3& aabb_min, glm::vec3& aabb_max, bool& aabb_seeded)
{
    const glm::mat3 normal_mat = glm::inverseTranspose(glm::mat3(world));
    const uint32_t base = static_cast<uint32_t>(verts.size());

    // For a matched glTF mesh, positions come straight from the file (assimp's Windows
    // build mis-reads a few, spiking them); assimp is the source only for its (untouched)
    // normals/UVs. A skinned mesh's `world` is identity, so file mesh-local == baked.
    const bool use_gltf_pos =
        gltf_skin && gltf_skin->valid && gltf_skin->positions.size() == mesh->mNumVertices;

    for (unsigned vi = 0; vi < mesh->mNumVertices; ++vi) {
        SceneVertex v{};
        const glm::vec3 lp = use_gltf_pos
            ? gltf_skin->positions[vi]
            : glm::vec3(mesh->mVertices[vi].x, mesh->mVertices[vi].y, mesh->mVertices[vi].z);
        const glm::vec3 wp = glm::vec3(world * glm::vec4(lp, 1.0f));
        v.pos = wp;
        if (mesh->HasNormals()) {
            const aiVector3D& n = mesh->mNormals[vi];
            v.normal = glm::normalize(normal_mat * glm::vec3(n.x, n.y, n.z));
        }
        if (mesh->HasTextureCoords(0)) {
            v.uv = glm::vec2(mesh->mTextureCoords[0][vi].x,
                             mesh->mTextureCoords[0][vi].y);
        }
        // boneIds / boneWeights left zero (SceneVertex v{}); the skin scatter below
        // fills them for skinned meshes, and a boneless mesh keeps the static path.
        verts.push_back(v);

        // Only finite positions grow the AABB: a corrupt file with a NaN/Inf vertex
        // would otherwise poison aabbMin/Max, and NaN bypasses SetAsset's radius
        // guard (NaN < eps is false) → a NaN camera and a silently black viewport.
        if (!(std::isfinite(wp.x) && std::isfinite(wp.y) && std::isfinite(wp.z)))
            continue;
        if (!aabb_seeded) { aabb_min = aabb_max = wp; aabb_seeded = true; }
        else { aabb_min = glm::min(aabb_min, wp); aabb_max = glm::max(aabb_max, wp); }
    }

    // glTF work-around path: weights were read straight from the file (top-4, already
    // global-indexed) because assimp mis-reads >4-influence glTF skins on Windows.
    // Copy them onto the verts just pushed; assimp's own (corrupt) weights are ignored.
    if (gltf_skin && gltf_skin->valid &&
        gltf_skin->ids.size() == mesh->mNumVertices) {
        for (unsigned vi = 0; vi < mesh->mNumVertices; ++vi) {
            verts[base + vi].boneIds     = gltf_skin->ids[vi];
            verts[base + vi].boneWeights = gltf_skin->weights[vi];
        }
    } else {
        // assimp path (FBX/Collada, or a glTF we couldn't match). Scatter skin
        // influences into the global bone slots (Task 2 / §C): assimp's per-mesh bone
        // index is local; remap to the global skeleton index via the §B name map so 3.3
        // can address palette[boneId]. mVertexId is in the mesh's own vertex index
        // space, so `base + id` lands on the vertex AppendMesh just pushed. aiProcess_
        // LimitBoneWeights has already capped each vertex to its 4 heaviest, so the
        // free-slot fill below keeps all of them.
        for (unsigned lb = 0; lb < mesh->mNumBones; ++lb) {
            const aiBone* b = mesh->mBones[lb];
            if (!b) continue;                        // null bone slot on a malformed file (AR18)
            const auto it = bone_index.find(b->mName.C_Str());
            if (it == bone_index.end()) continue;   // skeleton built first → always found
            const int gb = it->second;
            for (unsigned w = 0; w < b->mNumWeights; ++w) {
                const aiVertexWeight& vw = b->mWeights[w];
                // Reject zero/negative AND non-finite weights: a NaN passes `<= 0`
                // (compares false), then defeats the `== 0.0f` free-slot sentinel below
                // and would upload as a NaN-deformed vertex in 3.3.
                if (!std::isfinite(vw.mWeight) || vw.mWeight <= 0.0f) continue;
                const size_t v = base + vw.mVertexId;
                if (v >= verts.size()) continue;     // defensive against a stale vertex id
                SceneVertex& vert = verts[v];
                // Fill a free slot; if full, evict the smallest so the 4 kept are the
                // vertex's 4 heaviest (defensive if a source ever exceeds 4).
                int slot = -1;
                for (int s = 0; s < 4; ++s)
                    if (vert.boneWeights[s] == 0.0f) { slot = s; break; }
                if (slot >= 0) {
                    vert.boneIds[slot]     = gb;
                    vert.boneWeights[slot] = vw.mWeight;
                    continue;
                }
                int smallest = 0;
                for (int s = 1; s < 4; ++s)
                    if (vert.boneWeights[s] < vert.boneWeights[smallest]) smallest = s;
                if (vw.mWeight > vert.boneWeights[smallest]) {
                    vert.boneIds[smallest]     = gb;
                    vert.boneWeights[smallest] = vw.mWeight;
                }
            }
        }
    }

    // Triangle list — from the glTF file for a matched mesh (dodging any assimp index
    // corruption), else from assimp's faces. glTF indices are 0-based into this mesh,
    // so `base +` lands them on the verts just pushed.
    if (gltf_skin && gltf_skin->valid && !gltf_skin->indices.empty()) {
        // Validate per-triangle, not per-index: a lone out-of-range index must drop its
        // whole triangle, else removing one element shifts every following 3-vertex group
        // and scrambles the rest of the mesh. (LoadGltfSkin guarantees count % 3 == 0.)
        const std::vector<uint32_t>& gi = gltf_skin->indices;
        for (size_t t = 0; t + 3 <= gi.size(); t += 3) {
            if (gi[t] >= mesh->mNumVertices || gi[t + 1] >= mesh->mNumVertices ||
                gi[t + 2] >= mesh->mNumVertices) continue;
            indices.push_back(base + gi[t]);
            indices.push_back(base + gi[t + 1]);
            indices.push_back(base + gi[t + 2]);
        }
    } else {
        for (unsigned fi = 0; fi < mesh->mNumFaces; ++fi) {
            const aiFace& f = mesh->mFaces[fi];
            // Triangulate guarantees 3 indices/face; guard anyway against degenerate
            // points/lines assimp may keep.
            if (f.mNumIndices != 3) continue;
            indices.push_back(base + f.mIndices[0]);
            indices.push_back(base + f.mIndices[1]);
            indices.push_back(base + f.mIndices[2]);
        }
    }
}

// Recursively bakes node world transforms into model space (static path). Each
// (node, referenced-mesh) pair becomes one SceneMesh so instanced meshes keep
// their distinct transforms and per-material grouping is preserved. The node
// hierarchy itself is discarded here (Epic 3 keeps it for skinning instead).
// Story 11-2: the CPU mesh is now part of the shared CpuAsset (asset_loader.h).
using PendingMesh = CpuMesh;

void WalkBake(const aiScene* scene, const aiNode* node, const glm::mat4& parent_world,
              const std::unordered_map<std::string, int>& bone_index,
              const std::vector<ResolvedMeshSkin>& gltf_skins,
              std::vector<PendingMesh>& out,
              glm::vec3& aabb_min, glm::vec3& aabb_max, bool& aabb_seeded)
{
    const glm::mat4 world = parent_world * ConvertAssimpMatrix(node->mTransformation);

    for (unsigned i = 0; i < node->mNumMeshes; ++i) {
        // assimp normally keeps node mesh indices in range, but a corrupt/incomplete
        // file can carry a stale one; an out-of-range deref here is UB, not a throw
        // the LoadAsset catch could convert. Guard before indexing scene->mMeshes.
        const unsigned mesh_index = node->mMeshes[i];
        if (mesh_index >= scene->mNumMeshes) continue;
        const aiMesh* mesh = scene->mMeshes[mesh_index];
        if (!mesh) continue;
        PendingMesh pm;
        pm.materialIdx = mesh->mMaterialIndex;
        pm.skinned     = (mesh->mNumBones > 0);
        // A skinned mesh stays in mesh-local space (identity bake): the per-frame
        // palette globalMat*inverseBindMatrix already carries each vertex from
        // mesh-local to animated world space, so baking the node-world transform here
        // would apply the hierarchy twice and fold/explode the rig (§C). A static mesh
        // keeps world-baking exactly as in Epic 2 (byte-for-byte → AC3).
        const glm::mat4 bake = pm.skinned ? glm::mat4(1.0f) : world;
        const ResolvedMeshSkin* gskin =
            mesh_index < gltf_skins.size() ? &gltf_skins[mesh_index] : nullptr;
        AppendMesh(mesh, bake, bone_index, gskin, pm.verts, pm.indices,
                   aabb_min, aabb_max, aabb_seeded);
        if (!pm.verts.empty() && !pm.indices.empty())
            out.push_back(std::move(pm));
    }

    for (unsigned i = 0; i < node->mNumChildren; ++i)
        WalkBake(scene, node->mChildren[i], world, bone_index, gltf_skins, out,
                 aabb_min, aabb_max, aabb_seeded);
}

// The framing bounds of what is actually DRAWN (Epic 9). A skinned mesh is stored
// mesh-local (identity bake, §C) and only reaches world space through the bone palette,
// so the AABB WalkBake grows from its raw verts misses every transform the palette
// carries. Blender FBX is the canonical case: its Armature node is keyed at x100 scale +
// -90 deg X, so the drawn rig is 100x the raw bounds and the camera, zoom clamp, clip
// planes, floor and shadow (all sized from these bounds) framed a speck of it.
// Re-derive the bounds by CPU-skinning exactly like the vertex shader (same id clamp,
// same weight renormalization) over the clip the renderer plays (animations[0]), at
// evenly spaced times so root motion stays in frame too. Static meshes are already
// world-baked and contribute as-is. Returns false (outputs untouched) when the renderer
// would not skin either (channel/bone mismatch → static path): the raw bounds ARE then
// what is drawn.
bool ComputePosedBounds(const SceneSkeleton& skel, const SceneAnimation& clip,
                        const std::vector<PendingMesh>& meshes,
                        glm::vec3& out_min, glm::vec3& out_max)
{
    const size_t nb = skel.bones.size();
    if (nb == 0 || clip.channels.size() != nb) return false;  // == the renderer's pose_valid

    size_t skinned_verts = 0;
    for (const PendingMesh& pm : meshes)
        if (pm.skinned) skinned_verts += pm.verts.size();
    if (skinned_verts == 0) return false;

    // Load-time cost cap (~4M vertex skins): 32 poses for a typical rig, down to the two
    // clip ends for a multi-million-vertex mesh. A zero-length clip has a single pose.
    constexpr size_t kSkinBudget = 4000000;
    const int samples = (clip.duration > 0.0f)
        ? static_cast<int>(std::clamp<size_t>(kSkinBudget / skinned_verts, 2, 32))
        : 1;

    std::vector<glm::mat4> palette(nb, glm::mat4(1.0f));
    std::vector<glm::mat4> global(nb, glm::mat4(1.0f));
    // The shader clamps ids to [0,127] (only the first 128 matrices are uploaded); also
    // stay inside this palette for a rig with fewer bones.
    const int max_id = static_cast<int>(std::min<size_t>(nb, 128)) - 1;

    bool seeded = false;
    auto grow = [&](const glm::vec3& p) {
        // Same finite-only rule as AppendMesh: a NaN/Inf vertex must not poison the bounds.
        if (!(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z))) return;
        if (!seeded) { out_min = out_max = p; seeded = true; }
        else { out_min = glm::min(out_min, p); out_max = glm::max(out_max, p); }
    };

    for (const PendingMesh& pm : meshes)
        if (!pm.skinned)
            for (const SceneVertex& v : pm.verts) grow(v.pos);

    for (int s = 0; s < samples; ++s) {
        const float t = (samples > 1)
            ? clip.duration * static_cast<float>(s) / static_cast<float>(samples - 1)
            : 0.0f;
        ComputePose(skel, clip, t, palette, global);
        for (const PendingMesh& pm : meshes) {
            if (!pm.skinned) continue;
            for (const SceneVertex& v : pm.verts) {
                const glm::vec4& w = v.boneWeights;
                const float wsum = w.x + w.y + w.z + w.w;
                if (!(wsum > 0.0f)) { grow(v.pos); continue; }  // shader: identity skin
                const glm::vec4 p(v.pos, 1.0f);
                glm::vec4 acc(0.0f);
                for (int k = 0; k < 4; ++k)
                    acc += w[k] * (palette[std::clamp(v.boneIds[k], 0, max_id)] * p);
                grow(glm::vec3(acc) / wsum);
            }
        }
    }
    return seeded;
}

// Reads the skin weights of a .glb/.gltf straight from the file and maps them onto
// assimp's meshes — the work-around for the assimp 6.0.5 Windows glTF weight bug (see
// gltf_skin.h). Returns a per-scene-mesh table; every entry is `valid=false` (→ assimp
// weights) for non-glTF files, an unparseable glTF, or a mesh we cannot confidently
// pair to a file primitive. Matching is by vertex count + first vertex position, which
// is unambiguous because assimp keeps skinned verts mesh-local and in file order.
std::vector<ResolvedMeshSkin> ResolveGltfSkins(
    const aiScene* scene, const std::string& path,
    const std::unordered_map<std::string, int>& bone_index,
    SceneSkeleton& skeleton)
{
    std::vector<ResolvedMeshSkin> out(scene->mNumMeshes);

    auto ends_with_ci = [](const std::string& s, const char* suf) {
        const size_t n = std::strlen(suf);
        if (s.size() < n) return false;
        for (size_t i = 0; i < n; ++i)
            if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != suf[i])
                return false;
        return true;
    };
    if (!ends_with_ci(path, ".glb") && !ends_with_ci(path, ".gltf")) return out;

    const GltfSkinData g = LoadGltfSkin(path);
    if (!g.ok) {
        LogWarn("gltf skin: direct read failed - falling back to assimp weights");
        return out;
    }

    // Override each bone's inverse-bind matrix with the file's (assimp's Windows build
    // corrupts a few, spraying that bone's verts into needle-spikes). On a correct
    // assimp read these are identical, so this is a no-op except where it fixes damage.
    size_t ibm_fixed = 0;
    for (size_t local = 0; local < g.inverseBind.size() && local < g.jointNames.size(); ++local) {
        const auto it = bone_index.find(g.jointNames[local]);
        if (it == bone_index.end() || it->second >= static_cast<int>(skeleton.bones.size()))
            continue;
        glm::mat4& dst = skeleton.bones[it->second].inverseBindMatrix;
        if (!MatNearlyEqual(dst, g.inverseBind[local])) ++ibm_fixed;
        dst = g.inverseBind[local];
    }

    std::vector<bool> used(g.meshes.size(), false);
    unsigned matched = 0;
    size_t pos_fixed = 0;   // verts whose assimp position disagreed with the file
    for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* m = scene->mMeshes[mi];
        if (!m || m->mNumBones == 0 || m->mNumVertices == 0) continue;
        // Match by vertex count (unique here) first — assimp may have corrupted vertex 0's
        // position, so only lean on firstPos to break ties between same-count candidates.
        int found = -1, ncand = 0;
        for (size_t c = 0; c < g.meshes.size(); ++c) {
            if (used[c] || g.meshes[c].vertexCount != m->mNumVertices) continue;
            ++ncand; if (found < 0) found = static_cast<int>(c);
        }
        if (ncand > 1) {
            found = -1;
            const glm::vec3 p0(m->mVertices[0].x, m->mVertices[0].y, m->mVertices[0].z);
            for (size_t c = 0; c < g.meshes.size(); ++c)
                if (!used[c] && g.meshes[c].vertexCount == m->mNumVertices &&
                    glm::length(p0 - g.meshes[c].firstPos) < 1e-3f) { found = static_cast<int>(c); break; }
        }
        if (found < 0) continue;  // leave invalid → this mesh uses assimp's data
        used[found] = true;
        const GltfMeshSkin& ms = g.meshes[found];

        ResolvedMeshSkin& r = out[mi];
        r.ids.assign(m->mNumVertices, glm::ivec4(0));
        r.weights.assign(m->mNumVertices, glm::vec4(0.0f));
        r.positions = ms.positions;   // file positions replace assimp's (spike fix)
        r.indices   = ms.indices;     // file triangle list replaces assimp's
        for (unsigned vi = 0; vi < m->mNumVertices; ++vi) {
            for (int k = 0; k < 4; ++k) {
                const float w = ms.weight[vi][k];
                if (!(w > 0.0f)) continue;
                const int loc = ms.jointLocal[vi][k];
                if (loc < 0 || loc >= static_cast<int>(g.jointNames.size())) continue;
                const auto it = bone_index.find(g.jointNames[loc]);
                if (it == bone_index.end()) continue;   // joint not in skeleton (shouldn't happen)
                r.ids[vi][k]     = it->second;
                r.weights[vi][k] = w;
            }
            // Diagnostic: how far did assimp's position drift from the file's?
            const glm::vec3 ap(m->mVertices[vi].x, m->mVertices[vi].y, m->mVertices[vi].z);
            if (glm::length(ap - ms.positions[vi]) > 1e-3f) ++pos_fixed;
        }
        r.valid = true;
        ++matched;
    }
    if (matched)
        LogInfo("gltf skin: read geometry+weights directly for %u/%u mesh(es) "
                "(assimp glTF work-around; assimp got %zu vert positions and "
                "%zu inverse-bind matrices wrong)", matched, scene->mNumMeshes,
                pos_fixed, ibm_fixed);
    else if (g.ok)
        // We parsed skinned primitives from the file but paired none to an assimp mesh
        // (e.g. JoinIdenticalVertices welded verts so the vertex counts differ). The mesh
        // then silently keeps assimp's weights — the very data this path exists to replace.
        LogWarn("gltf skin: parsed the file but matched 0/%u assimp mesh(es) - using "
                "assimp weights (vertex-count mismatch? the assimp glTF bug may resurface)",
                scene->mNumMeshes);
    return out;
}

// Uploads one CPU mesh to a fresh VBO/IBO. Returns false (handles cleaned up by
// RAII) if GL could not allocate the buffers. PRECONDITION: current GL context.
bool UploadMesh(const PendingMesh& pm, SceneMesh& out)
{
    glGenBuffers(1, out.vb.addr());
    glGenBuffers(1, out.ib.addr());
    if (out.vb.get() == 0 || out.ib.get() == 0)
        return false;

    // Drain any pre-existing error so the post-upload check observes only THIS
    // upload (glGetError reports the oldest error and clears one flag — a stale one
    // would otherwise mask a real GL_OUT_OF_MEMORY from glBufferData below).
    while (glGetError() != GL_NO_ERROR) {}

    glBindBuffer(GL_ARRAY_BUFFER, out.vb.get());
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(pm.verts.size() * sizeof(SceneVertex)),
                 pm.verts.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, out.ib.get());
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(pm.indices.size() * sizeof(uint32_t)),
                 pm.indices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    // Any error from the upload (GL_OUT_OF_MEMORY for an oversized buffer, or
    // anything else) is a GPU failure; drain the whole queue so nothing leaks into
    // the next frame's error state.
    bool gl_failed = false;
    while (glGetError() != GL_NO_ERROR) gl_failed = true;
    if (gl_failed)
        return false;

    out.indexCount  = static_cast<uint32_t>(pm.indices.size());
    out.materialIdx = pm.materialIdx;
    out.skinned     = pm.skinned;  // data for 3.3's path selection; drives nothing yet
    return true;
}

bool FileExists(const std::string& path)
{
    // `path` is UTF-8 (the picker hands us CP_UTF8). u8path decodes it to the native
    // wide form so non-ASCII paths (e.g. an accented Windows user folder) resolve
    // correctly — a plain narrow ifstream would mangle them under the system codepage.
    // The error_code overload never throws (we are inside the no-throw boundary).
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::u8path(path), ec);
}

// Plain-language hint for a file assimp could not read (LoadResult::hint). Recognizes
// the common export mistakes we can tell apart from assimp's message and the file's
// first bytes; anything else gets the generic "damaged, export again" line. No-throw.
std::string ParseFailureHint(const std::string& path, const std::string& assimp_error)
try {
    if (assimp_error.find("old format version") != std::string::npos)
        return "This FBX format is too old. Re-export it as FBX 2013 or newer.";
    if (assimp_error.find("No suitable reader") != std::string::npos)
        return "Not a valid 3D file: it may be damaged, or have the wrong file extension.";

    // A text (ASCII) FBX starts with "; FBX ..." comments and FBXHeaderExtension, a
    // binary one with "Kaydara FBX Binary". assimp's ASCII reader is the fragile one.
    char head[1024] = {};
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    f.read(head, sizeof(head) - 1);
    const std::string start(head, static_cast<size_t>(f.gcount()));
    if (start.rfind("Kaydara FBX Binary", 0) != 0 &&
        (start.find("; FBX") != std::string::npos ||
         start.find("FBXHeaderExtension") != std::string::npos))
        return "This FBX is saved as text (ASCII). Re-export it as binary FBX.";

    return "This file couldn't be read. It may be damaged: try exporting it again.";
} catch (...) {
    // Also called from LoadAsset's catch handlers, where a throw would escape: degrade
    // to no hint (an empty string never allocates); the viewer shows its generic line.
    return {};
}

// assimp 6.0.5's FBX importer reuses one aiBone for every skin cluster that targets the
// same bone (seen in Unity exports), so mesh->mBones can list the same pointer twice.
// LimitBoneWeights then writes that bone's weights twice into an array sized for one
// copy: a heap overflow that crashed REAPER (issue #1, Skeleton_01.fbx). Drop the repeats
// before post-processing. assimp already kept only the first cluster's weights, so no
// weight is lost here. aiMesh's destructor de-duplicates its bones, so nothing leaks.
void DropDuplicateBones(const aiScene* scene)
{
    for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi) {
        aiMesh* mesh = scene->mMeshes[mi];
        if (!mesh || !mesh->mBones) continue;
        std::unordered_set<const aiBone*> seen;
        unsigned kept = 0;
        for (unsigned b = 0; b < mesh->mNumBones; ++b)
            if (mesh->mBones[b] && seen.insert(mesh->mBones[b]).second)
                mesh->mBones[kept++] = mesh->mBones[b];
        if (kept != mesh->mNumBones)
            LogWarn("mesh '%s': %u duplicate bone(s) dropped", mesh->mName.C_Str(),
                    mesh->mNumBones - kept);
        mesh->mNumBones = kept;
    }
}

}  // namespace

const char* LoadErrorCategoryName(LoadErrorCategory category)
{
    switch (category) {
        case LoadErrorCategory::Ok:                return "ok";
        case LoadErrorCategory::FileNotFound:      return "file-not-found";
        case LoadErrorCategory::ParseFailed:       return "parse-failed";
        case LoadErrorCategory::UnsupportedFormat: return "unsupported-format";
        case LoadErrorCategory::GpuUploadFailed:   return "gpu-upload-failed";
        case LoadErrorCategory::OutOfMemory:       return "out-of-memory";
        case LoadErrorCategory::Unknown:           return "unknown";
    }
    return "unknown";
}

CpuLoadResult LoadCpuAsset(const std::string& path)
{
    // assimp throws std::exception-derived on some malformed inputs; the whole body
    // is wrapped so nothing escapes this function (AR18). Catch std::bad_alloc as
    // OutOfMemory specifically, then any other std::exception as ParseFailed.
    // Story 11-2: GL-free (the upload is UploadAsset), so it runs on any thread.
    try {
        if (!FileExists(path))
            return {nullptr, LoadErrorCategory::FileNotFound, "cannot open " + path,
                    "File not found. It may have been moved, renamed or deleted."};

        Assimp::Importer importer;

        // Set now though glTF doesn't need it: the loader is shared with FBX from
        // Epic 7 and Mixamo rigs break without PreservePivots=0 (Spike Finding 3).
        importer.SetPropertyInteger(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, 0);

        // No MakeLeftHanded / FlipWindingOrder / PreTransformVertices — they would
        // defeat the "render as-authored" contract (D3/AR13) and PreTransform also
        // destroys the node hierarchy Epic 3 needs.
        // UV V-flip (Story 6.5.1 gate fix): we upload textures TOP-DOWN (stb row 0 = image
        // top), so GL samples v=0 at the image TOP. assimp delivers UVs in a BOTTOM-UP
        // convention (v=0 = image bottom) for EVERY importer — it flips glTF's top-left
        // origin to match FBX/OBJ/Collada's bottom-left, verified offline (assimp V =
        // 1 − raw-glTF V). So the reconciliation is format-INDEPENDENT: always flip V, or
        // every textured asset samples the mirrored-V location. On a UV atlas (a character,
        // or this steampunk body) that lands geometry on the wrong/padded texture region —
        // the "smeared / offset texture" both gates surfaced. (An earlier build flipped
        // only non-glTF, which left glTF/GLB V-flipped — exactly the GLB offset.)
        // Read raw, repair the duplicate bones, THEN post-process (DropDuplicateBones).
        const aiScene* scene = importer.ReadFile(path, 0);
        if (scene) {
            DropDuplicateBones(scene);
            scene = importer.ApplyPostProcessing(
                aiProcess_Triangulate | aiProcess_GenSmoothNormals |
                aiProcess_JoinIdenticalVertices | aiProcess_LimitBoneWeights |
                aiProcess_FlipUVs);
        }

        if (!scene || !scene->mRootNode) {
            const std::string err = importer.GetErrorString();
            return {nullptr, LoadErrorCategory::ParseFailed, "assimp: " + err,
                    ParseFailureHint(path, err)};
        }

        static const char kNoMeshHint[] =
            "No mesh in this file, only a skeleton or animation. "
            "Export the character mesh together with its skeleton.";

        // assimp's FBX importer flags a mesh-less scene (a skeleton/animation-only export)
        // INCOMPLETE without setting an error string, so name it here — otherwise the
        // copied error log would read "parse-failed" with an empty reason.
        if (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) {
            if (scene->mNumMeshes == 0)
                return {nullptr, LoadErrorCategory::UnsupportedFormat,
                        "file contains no meshes (skeleton/animation-only export?)",
                        kNoMeshHint};
            const std::string err = importer.GetErrorString();
            return {nullptr, LoadErrorCategory::ParseFailed,
                    "assimp flagged the scene incomplete: " + err,
                    ParseFailureHint(path, err)};
        }

        if (scene->mNumMeshes == 0)
            return {nullptr, LoadErrorCategory::UnsupportedFormat,
                    "file contains no meshes", kNoMeshHint};

        // Parse the skin skeleton first: WalkBake's per-vertex weight scatter needs
        // the bone-name -> global-index map. Empty for a boneless file (FR6 static
        // path) — the scatter then no-ops and skinned stays false everywhere.
        std::unordered_map<std::string, int> bone_index;
        std::vector<glm::mat4> bind_local;  // rest-pose local per bone — the unanimated
                                            // default-key source for ParseAnimations (§C)
        SceneSkeleton skeleton = BuildSkeleton(scene, bone_index, bind_local);

        // For glTF, read the skin weights straight from the file (assimp mis-reads
        // >4-influence glTF skins on Windows). Non-glTF / unmatched meshes get an
        // invalid entry and fall back to assimp's weights inside AppendMesh.
        const std::vector<ResolvedMeshSkin> gltf_skins =
            ResolveGltfSkins(scene, path, bone_index, skeleton);

        auto asset = std::make_shared<CpuAsset>();

        // Bake all node world transforms into model-space CPU meshes + union AABB.
        std::vector<PendingMesh>& pending = asset->meshes;
        glm::vec3 aabb_min(0.0f), aabb_max(0.0f);
        bool aabb_seeded = false;
        WalkBake(scene, scene->mRootNode, glm::mat4(1.0f), bone_index, gltf_skins, pending,
                 aabb_min, aabb_max, aabb_seeded);

        if (pending.empty())
            return {nullptr, LoadErrorCategory::UnsupportedFormat,
                    "file has meshes but no drawable geometry",
                    "The mesh in this file is empty (no visible geometry)."};

        asset->aabbMin = aabb_min;
        asset->aabbMax = aabb_max;
        // FBX comes out of assimp in centimetres whatever the file's own unit
        // (correctRootTransform scales the root by UnitScaleFactor); glTF and Collada
        // (unit size applied to the root) come out in metres.
        {
            std::string ext = std::filesystem::u8path(path).extension().u8string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            asset->metersPerUnit = (ext == ".fbx") ? 0.01f : 1.0f;
        }
        // modelRoot stays identity: canonical files render upright, non-canonical
        // render tilted/scaled as-authored (AR13), recovered later by Reset Camera.

        // Resolve each material's textures beside its flat factors. model_dir (computed
        // once) anchors the sibling-file branch; a per-texture decode/IO failure returns
        // an empty image and the load continues (only a catastrophic std::bad_alloc from
        // a pixel buffer reaches the OutOfMemory catch).
        const std::filesystem::path model_dir =
            std::filesystem::u8path(path).parent_path();
        asset->materials.reserve(scene->mNumMaterials);
        // Textures a material DECLARES but that came back empty — for the user notices:
        // external files not found are listed by name (distinct, case-insensitive: the
        // "Locate textures..." button looks for them, issue #1), the rest (undecodable)
        // counted. A material that declares no texture is the normal flat path.
        int missing_textures = 0;
        std::vector<std::string> missing_texture_files;
        for (unsigned mi = 0; mi < scene->mNumMaterials; ++mi) {
            const aiMaterial* aimat = scene->mMaterials[mi];
            const SceneMaterial factors = ConvertMaterial(aimat);
            CpuMaterial material;
            material.baseColorFactor = factors.baseColorFactor;
            material.specularColor   = factors.specularColor;
            material.shininess       = factors.shininess;
            // One not-found name per slot (empty = found, or not an external file).
            std::string nf_base, nf_normal, nf_spec, nf_gloss;
            auto count_if_missing = [&](const CpuImage& img, aiTextureType type,
                                        const std::string& not_found) {
                aiString declared;
                if (!img.empty() || !aimat ||
                    aimat->GetTexture(type, 0, &declared) != AI_SUCCESS)
                    return;
                if (not_found.empty()) {
                    ++missing_textures;
                    return;
                }
                for (const std::string& n : missing_texture_files)
                    if (SameFileNameNoCase(n, not_found)) return;
                missing_texture_files.push_back(not_found);
            };
            // Base colour is a COLOUR texture → sRGB-decoded on sample (GL_SRGB8_ALPHA8,
            // chosen at upload) so lighting math runs in linear space (Story 6.5.1 AC1).
            // The normal map is DATA → uploaded linear (GL_RGBA8) and only sampled when
            // present (AC4).
            material.baseColor =
                ResolveTexture(scene, aimat, aiTextureType_DIFFUSE, model_dir, mi, "diffuse",
                               &nf_base);
            material.normalMap = ResolveNormalMap(scene, aimat, model_dir, mi, &nf_normal);
            // Story 6.5.7 — the artist's per-pixel specular + glossiness maps. Both are DATA
            // (reflection intensity / sharpness scalar), not colour → uploaded LINEAR
            // (GL_RGBA8), same rule as the normal map (AC6). Read directly via the existing
            // funnel (SPECULAR/SHININESS are plain texture slots, unlike the NORMALS-only
            // ResolveNormalMap), which already handles embedded/external + the
            // LogWarn-then-empty fallback (AR17). An empty image (glTF metallic-roughness, or
            // any mesh with no such slot) leaves the renderer on the uniform-sheen path (AC3).
            material.specularMap =
                ResolveTexture(scene, aimat, aiTextureType_SPECULAR, model_dir, mi, "specular",
                               &nf_spec);
            material.glossMap =
                ResolveTexture(scene, aimat, aiTextureType_SHININESS, model_dir, mi, "glossiness",
                               &nf_gloss);
            count_if_missing(material.baseColor,   aiTextureType_DIFFUSE,   nf_base);
            count_if_missing(material.normalMap,   aiTextureType_NORMALS,   nf_normal);
            count_if_missing(material.specularMap, aiTextureType_SPECULAR,  nf_spec);
            count_if_missing(material.glossMap,    aiTextureType_SHININESS, nf_gloss);
            asset->materials.push_back(std::move(material));
        }
        // assimp always emits a default material, but a mesh's materialIdx indexes
        // this list on the render hot path — guarantee at least one entry so a
        // malformed file can never drive an out-of-bounds read.
        if (asset->materials.empty()) {
            const SceneMaterial factors = ConvertMaterial(nullptr);
            CpuMaterial material;
            material.baseColorFactor = factors.baseColorFactor;
            material.specularColor   = factors.specularColor;
            material.shininess       = factors.shininess;
            asset->materials.push_back(std::move(material));
        }

        // ...and clamp any out-of-range index (a hand-edited/corrupt file can set
        // aiMesh::mMaterialIndex past mNumMaterials) to the guaranteed [0] entry, so
        // the renderer's materials[mesh.materialIdx] is always in bounds. The
        // empty-guard above only covers the zero-material case, not this one.
        const uint32_t material_count = static_cast<uint32_t>(asset->materials.size());
        for (PendingMesh& pm : pending)
            if (pm.materialIdx >= material_count) pm.materialIdx = 0;

        // Audit the parsed skeleton (skinned files only — a static load stays silent).
        // skinned_verts also feeds the "mesh not skinned" user notice below.
        size_t skinned_verts = 0;
        if (!skeleton.bones.empty()) {
            for (const PendingMesh& pm : pending)
                for (const SceneVertex& v : pm.verts)
                    if (v.boneWeights[0] != 0.0f || v.boneWeights[1] != 0.0f ||
                        v.boneWeights[2] != 0.0f || v.boneWeights[3] != 0.0f)
                        ++skinned_verts;
            DumpSkeleton(skeleton, skinned_verts);
        }

        // Parse the animation clip into channels + audit the sampler (animated files
        // only).
        bool any_bone_animated = false;  // for the "animation doesn't match" notice
        if (scene->mNumAnimations > 0 && !skeleton.bones.empty()) {
            std::vector<bool> animated;
            double tps = 0.0;
            asset->animations =
                ParseAnimations(scene, skeleton, bone_index, bind_local, animated, tps);
            if (!asset->animations.empty())
                DumpAnimation(skeleton, asset->animations.front(), animated, tps);
            for (bool a : animated) any_bone_animated |= a;
        }

        // Frame what is DRAWN, not the raw mesh-local skinned verts (Epic 9 — see
        // ComputePosedBounds). Kept as-is when the renderer won't skin either.
        if (!asset->animations.empty()) {
            glm::vec3 posed_min(0.0f), posed_max(0.0f);
            if (ComputePosedBounds(skeleton, asset->animations.front(), pending,
                                   posed_min, posed_max)) {
                asset->aabbMin = posed_min;
                asset->aabbMax = posed_max;
            }
        }
        asset->skeleton = std::move(skeleton);

        // The facts behind the user notices (built by LoadAsset, in the user's words).
        asset->no_animation       = (scene->mNumAnimations == 0);
        asset->mesh_not_skinned   = !asset->no_animation && skinned_verts == 0;
        asset->animation_mismatch = !asset->no_animation && skinned_verts != 0 && !any_bone_animated;
        asset->missing_textures   = missing_textures;
        asset->missing_texture_files = std::move(missing_texture_files);

        CpuLoadResult ok;
        ok.asset    = std::move(asset);
        ok.category = LoadErrorCategory::Ok;
        return ok;
    }
    catch (const std::bad_alloc&) {
        return {nullptr, LoadErrorCategory::OutOfMemory, "out of memory while loading",
                "Not enough memory to open this file."};
    }
    catch (const std::exception& e) {
        return {nullptr, LoadErrorCategory::ParseFailed, e.what(),
                ParseFailureHint(path, e.what())};
    }
    catch (...) {
        return {nullptr, LoadErrorCategory::Unknown, "unknown loader failure",
                "Unexpected error while opening this file."};
    }
}

bool UploadAsset(const CpuAsset& cpu, Asset& out, int* out_failed_textures)
{
    // The GPU half of the load (PRECONDITION: a current GL context). Built into a local
    // Asset and moved out only on success, so a failure leaves `out` untouched-empty and
    // frees the partial GL objects (RAII) while the context is still current.
    int failed = 0;
    try {
        Asset asset;
        asset.aabbMin       = cpu.aabbMin;
        asset.aabbMax       = cpu.aabbMax;
        asset.metersPerUnit = cpu.metersPerUnit;
        asset.modelRoot     = cpu.modelRoot;

        asset.materials.reserve(cpu.materials.size());
        for (size_t i = 0; i < cpu.materials.size(); ++i) {
            const CpuMaterial& cm = cpu.materials[i];
            const unsigned mi = static_cast<unsigned>(i);
            SceneMaterial m;
            m.baseColorFactor = cm.baseColorFactor;
            m.specularColor   = cm.specularColor;
            m.shininess       = cm.shininess;
            m.baseColor   = UploadCpuImage(cm.baseColor,   GL_SRGB8_ALPHA8, "diffuse",    mi, failed);
            m.normalMap   = UploadCpuImage(cm.normalMap,   GL_RGBA8,        "normal",     mi, failed);
            m.specularMap = UploadCpuImage(cm.specularMap, GL_RGBA8,        "specular",   mi, failed);
            m.glossMap    = UploadCpuImage(cm.glossMap,    GL_RGBA8,        "glossiness", mi, failed);
            asset.materials.push_back(std::move(m));
        }
        if (asset.materials.empty()) asset.materials.push_back(ConvertMaterial(nullptr));

        asset.meshes.reserve(cpu.meshes.size());
        for (const CpuMesh& pm : cpu.meshes) {
            SceneMesh sm;
            if (!UploadMesh(pm, sm)) {
                if (out_failed_textures) *out_failed_textures = failed;
                return false;
            }
            // The material list may differ from cpu.materials only in the empty case
            // (one fallback entry), where every index was clamped to 0 by the parse.
            if (sm.materialIdx >= asset.materials.size()) sm.materialIdx = 0;
            asset.meshes.push_back(std::move(sm));
        }

        asset.skeleton   = cpu.skeleton;    // copied: the GPU copy owns its pose data
        asset.animations = cpu.animations;
        out = std::move(asset);
    } catch (...) {
        // bad_alloc copying the skeleton/animations: no GPU copy (the RAII handles
        // already freed whatever was uploaded, with the context current).
        if (out_failed_textures) *out_failed_textures = failed;
        return false;
    }
    if (out_failed_textures) *out_failed_textures = failed;
    return true;
}

LoadResult LoadAsset(const std::string& path)
{
    // Parse (GL-free) then upload into the current context. No-throw (AR18).
    CpuLoadResult parsed = LoadCpuAsset(path);
    if (!parsed.asset)
        return {std::nullopt, parsed.category, std::move(parsed.detail), std::move(parsed.hint)};

    const CpuAsset& cpu = *parsed.asset;
    try {
        Asset asset;
        int failed_textures = 0;
        if (!UploadAsset(cpu, asset, &failed_textures))
            return {std::nullopt, LoadErrorCategory::GpuUploadFailed,
                    "GL buffer upload failed (out of GPU memory?)",
                    "Your graphics card ran out of memory for this model."};

        // Non-blocking problems, in the user's words (LoadResult::notices). Each is also
        // kept in the copyable log (tagged with the file by the caller's log context).
        // Order: what the user notices first (the character not moving) comes first.
        std::vector<std::string> notices;
        if (cpu.no_animation)
            notices.push_back("No animation in this file: the character stays in its rest pose.");
        else if (cpu.mesh_not_skinned)
            notices.push_back("The mesh isn't skinned to a skeleton: it won't follow the animation.");
        else if (cpu.animation_mismatch)
            notices.push_back("The animation doesn't match this skeleton (different bone names): "
                              "the character won't move.");
        // Issue #1: texture files not found — the usual cause is REAPER copying only the
        // .fbx into the project. The viewer offers "Locate textures..." under this notice.
        const size_t not_found = cpu.missing_texture_files.size();
        if (not_found > 0) {
            notices.push_back(std::to_string(not_found) +
                              (not_found == 1 ? " texture file" : " texture files") +
                              " not found next to the model: shown in plain colour. If REAPER "
                              "copies imported media into your project folder, the texture "
                              "files must be copied there too.");
            std::string names = "Missing: ";
            for (size_t i = 0; i < not_found; ++i)
                names += (i ? ", " : "") + cpu.missing_texture_files[i];
            notices.push_back(std::move(names));
        }
        const int unreadable = cpu.missing_textures + failed_textures;
        if (unreadable > 0)
            notices.push_back(std::to_string(unreadable) +
                              (unreadable == 1 ? " texture" : " textures") +
                              " could not be read (unsupported or damaged image): shown in "
                              "plain colour.");
        for (const std::string& n : notices) LogWarn("notice: %s", n.c_str());

        LoadResult ok{std::move(asset), LoadErrorCategory::Ok, {}, {}};
        ok.notices = std::move(notices);
        ok.missing_texture_files = cpu.missing_texture_files;
        ok.cpu     = std::move(parsed.asset);
        return ok;
    }
    catch (const std::bad_alloc&) {
        return {std::nullopt, LoadErrorCategory::OutOfMemory, "out of memory while loading",
                "Not enough memory to open this file."};
    }
    catch (...) {
        return {std::nullopt, LoadErrorCategory::Unknown, "unknown loader failure",
                "Unexpected error while opening this file."};
    }
}

double ProbeAnimationDuration(const std::string& path)
{
    // Same no-throw envelope as LoadAsset (AR18): assimp throws on some malformed
    // inputs and this runs from a Reaper-driven boundary (the PCM_source ctor /
    // SetFileName on a file drop). Any escape -> 0.0, which the source maps to the
    // 1.0 s placeholder. Deliberately NO GL here (no UploadMesh) — there is no
    // current GL context on drop; this is the entire reason it is separate from
    // LoadAsset.
    try {
        if (!FileExists(path)) return 0.0;

        Assimp::Importer importer;

        // Pass 0 post-process flags: mDuration / mTicksPerSecond live on aiAnimation
        // and need ZERO mesh processing. Skipping Triangulate/GenSmoothNormals/etc.
        // is the NFR-P2 win (AC4) — the probe does only the cheap parse for duration.
        const aiScene* scene = importer.ReadFile(path, 0);

        // Kept for "Copy error log": a file that fails here only shows up as a 1 s item.
        if (!scene) {
            LogWarn("duration probe could not read %s: %s", path.c_str(),
                    importer.GetErrorString());
            return 0.0;
        }
        if ((scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || scene->mNumAnimations == 0)
            return 0.0;

        const aiAnimation* a = scene->mAnimations[0];
        if (!a) return 0.0;  // null slot guard — a raw deref is SEH/UB, not catchable

        // Ticks -> seconds with the identical finite-AND-positive guard vetted in
        // ParseAnimations (a bare != 0 lets a negative rate sign-flip the length and
        // lets NaN through). 25 is assimp's own fallback rate.
        double tps = a->mTicksPerSecond;
        if (!(tps > 0.0) || !std::isfinite(tps)) tps = 25.0;
        const double seconds = a->mDuration / tps;
        // Guard the RESULT finite, not just tps: a corrupt clip whose mDuration is
        // +Inf (or a value that overflows the divide) survives std::max(0.0, +Inf)
        // as +Inf, and GetLength()'s `m_len > 0.0` test PASSES for +Inf -> Reaper
        // gets an infinite-length item. Non-finite -> 0.0 -> the 1 s fallback (AC3).
        if (!std::isfinite(seconds)) return 0.0;
        return std::max(0.0, seconds);  // clamp >= 0: never a negative length
    }
    catch (...) {
        return 0.0;
    }
}

}  // namespace rav

#endif  // _WIN32
