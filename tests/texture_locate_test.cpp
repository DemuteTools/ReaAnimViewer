// SPDX-License-Identifier: MIT
//
// Host test of "Locate textures..." (src/texture_locate.h, GitHub issue #1): the texture
// file-name extraction and the search + copy on a temporary folder tree. No REAPER, no
// Windows: any C++17 compiler.
//   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests

#include "texture_locate.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <system_error>
#include <vector>

using namespace rav;
namespace fs = std::filesystem;

namespace {

int g_fails = 0;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++g_fails;                                              \
        }                                                           \
    } while (0)

void WriteFile(const fs::path& p, const std::string& content)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << content;
}

std::string ReadFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool Has(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

// A fresh empty folder under the system temp folder, removed by the destructor.
struct TempTree {
    fs::path root;
    TempTree()
    {
        std::random_device rd;
        root = fs::temp_directory_path() / ("rav_texture_locate_test_" + std::to_string(rd()));
        fs::remove_all(root);
        fs::create_directories(root / "project");
        fs::create_directories(root / "source");
    }
    ~TempTree()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    std::string Dest() const { return (root / "project").u8string(); }
    std::string Src() const { return (root / "source").u8string(); }
};

void TestFileName()
{
    CHECK(TextureFileName("..\\..\\sourceimages\\Substance\\V1\\T__Eidolon.png") == "T__Eidolon.png");
    CHECK(TextureFileName("../textures/a.png") == "a.png");
    CHECK(TextureFileName("C:\\x/y\\z.tga") == "z.tga");
    CHECK(TextureFileName("plain.jpg") == "plain.jpg");
    CHECK(TextureFileName("dir\\") == "");
    CHECK(TextureFileName("") == "");

    // ResolveTexture's tries: the stored path, then the file name beside the model.
    using V = std::vector<std::string>;
    CHECK(TextureCandidates("..\\..\\x\\T.png") == (V{"..\\..\\x\\T.png", "T.png"}));
    CHECK(TextureCandidates("../tex/T.png") == (V{"../tex/T.png", "T.png"}));
    CHECK(TextureCandidates("T.png") == (V{"T.png"}));
    CHECK(TextureCandidates("dir\\") == (V{"dir\\"}));

    CHECK(SameFileNameNoCase("T__Eidolon.PNG", "t__eidolon.png"));
    CHECK(!SameFileNameNoCase("a.png", "a.pn"));
    CHECK(!SameFileNameNoCase("a.png", "b.png"));
}

void TestMatchCaseInsensitive()
{
    TempTree t;
    WriteFile(t.root / "source" / "sub" / "t__EIDOLON.Png", "eidolon");
    const LocateResult r = LocateTextures(t.Dest(), {"T__Eidolon.png"}, t.Src());
    CHECK(r.copied.size() == 1 && Has(r.copied, "T__Eidolon.png"));
    CHECK(r.still_missing.empty() && r.failed.empty() && r.already_present.empty());
    // Copied under the name the loader looks for.
    CHECK(ReadFile(t.root / "project" / "T__Eidolon.png") == "eidolon");
}

void TestPartialAndShallowest()
{
    TempTree t;
    WriteFile(t.root / "source" / "a" / "b" / "one.png", "deep");
    WriteFile(t.root / "source" / "c" / "one.png", "shallow");
    const LocateResult r = LocateTextures(t.Dest(), {"one.png", "two.png"}, t.Src());
    CHECK(Has(r.copied, "one.png") && r.copied.size() == 1);
    CHECK(Has(r.still_missing, "two.png") && r.still_missing.size() == 1);
    CHECK(ReadFile(t.root / "project" / "one.png") == "shallow");
}

void TestDepthLimit()
{
    TempTree t;
    // depth = number of sub-folders below the picked folder.
    WriteFile(t.root / "source" / "1" / "2" / "3" / "4" / "at4.png", "x");
    WriteFile(t.root / "source" / "1" / "2" / "3" / "4" / "5" / "at5.png", "x");
    const LocateResult r = LocateTextures(t.Dest(), {"at4.png", "at5.png"}, t.Src());
    CHECK(Has(r.copied, "at4.png"));
    CHECK(Has(r.still_missing, "at5.png"));
    CHECK(!fs::exists(t.root / "project" / "at5.png"));
}

void TestNoOverwrite()
{
    TempTree t;
    WriteFile(t.root / "project" / "keep.png", "original");
    WriteFile(t.root / "source" / "keep.png", "other");
    const LocateResult r = LocateTextures(t.Dest(), {"keep.png"}, t.Src());
    CHECK(Has(r.already_present, "keep.png") && r.copied.empty() && r.failed.empty());
    CHECK(ReadFile(t.root / "project" / "keep.png") == "original");

    // A second run after a copy reports the file as already present.
    WriteFile(t.root / "source" / "new.png", "n");
    const LocateResult first = LocateTextures(t.Dest(), {"new.png"}, t.Src());
    CHECK(Has(first.copied, "new.png"));
    const LocateResult second = LocateTextures(t.Dest(), {"new.png"}, t.Src());
    CHECK(Has(second.already_present, "new.png") && second.copied.empty());
}

void TestNoneFound()
{
    TempTree t;
    WriteFile(t.root / "source" / "unrelated.png", "u");
    const LocateResult r = LocateTextures(t.Dest(), {"a.png", "b.png", "A.PNG"}, t.Src());
    CHECK(r.copied.empty() && r.failed.empty() && r.already_present.empty());
    CHECK(r.still_missing.size() == 2);   // a.png and A.PNG are one name
    CHECK(fs::is_empty(t.root / "project"));

    // A folder that does not exist: everything still missing, nothing thrown.
    const LocateResult r2 = LocateTextures(t.Dest(), {"a.png"}, (t.root / "nope").u8string());
    CHECK(r2.still_missing.size() == 1 && r2.copied.empty());
}

void TestCopyFails()
{
    TempTree t;
    WriteFile(t.root / "source" / "a.png", "a");
    // Destination folder missing → the copy fails and the name is reported.
    const LocateResult r = LocateTextures((t.root / "no_such_dir").u8string(), {"a.png"}, t.Src());
    CHECK(Has(r.failed, "a.png") && r.copied.empty());
}

}  // namespace

int main()
{
    TestFileName();
    TestMatchCaseInsensitive();
    TestPartialAndShallowest();
    TestDepthLimit();
    TestNoOverwrite();
    TestNoneFound();
    TestCopyFails();
    if (g_fails == 0) std::printf("texture_locate: all passed\n");
    return g_fails == 0 ? 0 : 1;
}
