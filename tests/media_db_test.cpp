// SPDX-License-Identifier: MIT
//
// Host test of src/media_db.h (story 13-5). Optional: `media_db_test <MediaDB folder>` loads a
// real folder and prints timings (D9; never run by ctest).

#include "media_db.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using namespace rav;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
            ++g_fail;                                                     \
        }                                                                 \
    } while (0)

static std::string ReadAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void WriteAll(const fs::path& p, const std::string& s)
{
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << s;
}

static DbFile F(const std::string& path, int64_t size, double len)
{
    DbFile f;
    f.path = path;
    f.size = size;
    f.length = len;
    return f;
}

static AssetIdentity Id(const std::string& name, int64_t size, double len, const std::string& last = "")
{
    AssetIdentity a;
    a.name = name;
    a.size = size;
    a.length = len;
    a.last_path = last;
    return a;
}

static const std::string kData = RAV_TEST_DATA "/mediadb";

static void TestLength()
{
    double s = 0;
    CHECK(ParseDbLength("0.25", &s) && s == 0.25);
    CHECK(ParseDbLength("7:53.666", &s) && std::fabs(s - 473.666) < 1e-9);
    CHECK(ParseDbLength("6:37:05.088", &s) && std::fabs(s - 23825.088) < 1e-9);
    CHECK(!ParseDbLength("abc", &s));
    CHECK(!ParseDbLength("", &s));
    CHECK(!ParseDbLength("1:2:3:4", &s));
    CHECK(!ParseDbLength("1::2", &s));
    CHECK(!ParseDbLength("-3", &s));
}

// D3: the three spike B entries; size - l*rate*ch*bits/8 = the WAV header (positive, < 64 KiB).
static void TestSpikeArithmetic()
{
    const std::string text =
        "FILE \"e1.wav\" 68212732 0 1770238095 0\n"
        "DATA \"d:ORGANIC BubbleWrap\" s:48000 n:1 l:7:53.666\nDATA i:24\n"
        "FILE \"e2.wav\" 135844732 0 1770238043 0\n"
        "DATA \"d:ORGANIC BubbleWrap\" s:48000 n:2 l:7:51.666\nDATA i:24\n"
        "FILE \"e3.wav\" 136420732 0 1770238086 0\n"
        "DATA \"d:ORGANIC BubbleWrap\" s:48000 n:2 l:7:53.666\nDATA i:24\n";
    std::vector<DbFile> f;
    size_t bad = 99;
    CHECK(ParseReaperFileList(text, "x", &f, &bad));
    CHECK(bad == 0 && f.size() == 3);
    const long expect[] = {4828, 4924, 4924};
    for (size_t i = 0; i < f.size() && i < 3; ++i) {
        const double pcm = f[i].length * f[i].rate * f[i].channels * f[i].bits / 8.0;
        const double hdr = static_cast<double>(f[i].size) - pcm;
        std::printf("  D3 entry %zu: header = %.1f bytes\n", i + 1, hdr);
        CHECK(std::fabs(hdr - expect[i]) < 0.5);
        CHECK(hdr > 0 && hdr < 65536);
        // A seconds-only or ms reading of the raw text fails it.
        CHECK(static_cast<double>(f[i].size) - 7.0 * 48000 * f[i].channels * 3 > 65536);
    }
}

static void TestParseFixture()
{
    std::vector<DbFile> f;
    size_t bad = 0;
    CHECK(ParseReaperFileList(ReadAll(kData + "/MediaDB/00.ReaperFileList"), "Foley Bank", &f, &bad));
    CHECK(bad == 3);  // DATA before FILE, WEIRD, unterminated quote
    CHECK(f.size() == 6);
    if (f.size() == 6) {
        CHECK(f[0].path == "C:\\Sounds\\Foley\\step_a.wav" && f[0].size == 48044 && f[0].length == 0.25);
        CHECK(f[0].mtime == 1700000000 && f[0].channels == 2 && f[0].rate == 48000 && f[0].bits == 24);
        CHECK(f[0].description == "Gravel footstep, heel first" && f[0].library == "Foley Lib");
        CHECK(f[1].path == "C:\\Sounds\\Foley\\FS Dirt 01.wav");
        CHECK(f[2].path == "C:\\Sounds\\Foley\\it's quoted.wav" && f[2].length == 0.0625);
        CHECK(f[3].length == 1.0 && f[4].length == 1.0005);
        CHECK(f[5].size == 5000000000LL && std::fabs(f[5].length - 23825.088) < 1e-6 && f[5].channels == 8);
        CHECK(f[0].db == "Foley Bank");
    }
    // Quote kinds
    f.clear();
    CHECK(ParseReaperFileList("FILE 'C:\\a b.wav' 1 0 0 0\nFILE `C:\\c\"d.wav` 2 0 0 0\nFILE \"e.wav\" 3\n", "", &f, &bad));
    CHECK(bad == 0 && f.size() == 3);
    if (f.size() == 3) CHECK(f[0].path == "C:\\a b.wav" && f[1].path == "C:\\c\"d.wav" && f[2].size == 3);
    // Empty / not a list
    f.clear();
    CHECK(ParseReaperFileList("", "", &f, &bad) && f.empty() && bad == 0);
    CHECK(!ParseReaperFileList("hello world\n", "", &f, &bad));
    CHECK(!ParseReaperFileList("FILE\n", "", &f, &bad) && bad == 1);
    // A broken FILE line: its DATA is unparsed, never attached to the file before it.
    f.clear();
    CHECK(ParseReaperFileList("FILE a.wav 1 0 0 0\nDATA \"l:1.0\"\nFILE \"broken\nDATA \"l:9.0\"\nFILE\nDATA \"l:8.0\"\n", "",
                              &f, &bad));
    CHECK(f.size() == 1 && bad == 4);
    if (f.size() == 1) CHECK(f[0].length == 1.0);
    // Cancel: checked every 4096 lines of the stream parser.
    std::string many;
    for (int i = 0; i < 5000; ++i) many += "FILE C:\\x\\" + std::to_string(i) + ".wav 1 0 0 0\n";
    std::atomic<bool> cancel{true};
    std::istringstream in(many);
    f.clear();
    CHECK(!ParseReaperFileList(in, "", &f, &bad, &cancel) && f.size() < 5000);
}

static void TestIni()
{
    const std::string res = "R";
    auto s = ListMediaDatabases(res, ReadAll(kData + "/reaper_sexplorer.ini"));
    CHECK(s.size() == 2);
    if (s.size() == 2) {
        CHECK(s[0].name == "Foley Bank" && s[0].path == (fs::path("R") / "MediaDB" / "00.ReaperFileList").u8string());
        CHECK(s[1].name == "05" && s[1].path == "C:\\Other\\05.ReaperFileList");
    }
    auto e = ListMediaDatabases(res, ReadAll(kData + "/reaper_explorer.ini"));
    CHECK(e.size() == 3);
    if (e.size() == 3) CHECK(e[0].name == "Foley Bank" && e[1].name == "Twin Bank" && e[2].name == "02");
    CHECK(ListMediaDatabases(res, "[reaper]\nx=1\n").empty());
    CHECK(ListMediaDatabases(res, "[reaper_explorer]\nshortcut=x.ReaperFileList\nShortcutT=y\n").empty());
}

static void TestFind()
{
    MediaDbIndex idx;
    idx.Add(F("C:/S/Hit.WAV", 100, 1.0));
    CHECK(idx.Find(Id("hit.wav", 100, 1.0008)).status == LookupStatus::Found);
    CHECK(idx.Find(Id("hit.wav", 100, 1.0008)).path == "C:/S/Hit.WAV");
    CHECK(idx.Find(Id("hit.wav", 100, 1.002)).status == LookupStatus::Missing);  // 2 ms off
    CHECK(idx.Find(Id("nope.wav", 100, 1.0)).status == LookupStatus::Missing);
    CHECK(idx.Find(Id("", -1, -1)).status == LookupStatus::Missing);
    CHECK(idx.Find(Id("hit.wav", -1, -1)).status == LookupStatus::Found);  // wildcards
    CHECK(idx.Find(Id("hit.wav", 101, -1)).status == LookupStatus::Missing);

    MediaDbIndex t;
    t.Add(F("E:/Bank/FS/a.wav", 2000, 0.5));
    t.Add(F("E:/Bank/Copy/a.wav", 2000, 0.5));
    LookupResult r = t.Find(Id("a.wav", 2000, 0.5, "D:\\Old\\Bank\\FS\\a.wav"));
    CHECK(r.status == LookupStatus::Found && r.path == "E:/Bank/FS/a.wav");
    r = t.Find(Id("a.wav", 2000, 0.5, "D:\\Old\\a.wav"));
    CHECK(r.status == LookupStatus::Ambiguous && r.candidates.size() == 2);
    AssetIdentity rel = Id("a.wav", 2000, 0.5);
    rel.rel_path = "Copy/a.wav";
    r = t.Find(rel);
    CHECK(r.status == LookupStatus::Found && r.path == "E:/Bank/Copy/a.wav");
    // The same file listed by two databases is one candidate.
    MediaDbIndex d;
    d.Add(F("E:/Bank/FS/z.wav", 10, 0.5));
    d.Add(F("e:\\bank\\fs\\z.wav", 10, 0.5));
    CHECK(d.Find(Id("z.wav", 10, 0.5, "D:\\Old\\z.wav")).status == LookupStatus::Found);
}

static void TestSearch()
{
    MediaDbIndex idx;
    std::vector<DbFile> f;
    ParseReaperFileList(ReadAll(kData + "/MediaDB/00.ReaperFileList"), "Foley Bank", &f, nullptr);
    ParseReaperFileList(ReadAll(kData + "/MediaDB/01.ReaperFileList"), "Twin Bank", &f, nullptr);
    for (const auto& x : f) idx.Add(x);
    CHECK(idx.Search("FOOTSTEP", "", 100).size() == 2);
    CHECK(idx.Search("footstep gravel", "", 100).size() == 1);
    CHECK(idx.Search("wood step", "Twin Bank", 100).size() == 2);
    CHECK(idx.Search("wood", "Foley Bank", 100).empty());
    CHECK(idx.Search("", "", 3).size() == 3);
    CHECK(idx.Search("dup.wav", "", 100).size() == 2);
}

static fs::path TempDir(const char* tag)
{
    // Unique per run: parallel builds share the temp folder.
    const long long stamp = static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::path d = fs::temp_directory_path() / (std::string("rav_media_db_") + tag + "_" + std::to_string(stamp));
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

static void TestRelocateAndScan()
{
    const fs::path root = TempDir("scan");
    WriteAll(root / "New" / "FS" / "a.wav", std::string(10, 'a'));
    WriteAll(root / "New" / "FS" / "b.wav", std::string(20, 'b'));
    WriteAll(root / "New" / "FS" / "c.wav", std::string(31, 'c'));  // size changed
    WriteAll(root / "New" / "FS" / "d.WAV", std::string(40, 'd'));
    WriteAll(root / "New" / "FS" / "notes.txt", "x");
    size_t calls = 0;
    auto length_of = [&calls](const std::string&) {
        ++calls;
        return 1.0;
    };
    const std::string res = (root / "res").u8string();
    auto lister = MakeCachedFolderLister(res, length_of);

    const std::string old = "D:\\Old\\FS\\";
    std::vector<AssetIdentity> miss = {Id("b.wav", 20, 1.0, old + "b.wav"), Id("c.wav", 30, 1.0, old + "c.wav"),
                                       Id("d.wav", 40, -1, old + "d.wav"), Id("e.wav", 40, -1, "D:\\Else\\e.wav")};
    const std::string located = (root / "New" / "FS" / "a.wav").u8string();
    auto rr = RelocateFolder(Id("a.wav", 10, 1.0, old + "a.wav"), located, miss, lister);
    CHECK(rr.size() == 4);
    if (rr.size() == 4) {
        CHECK(rr[0].status == LookupStatus::Found && fs::u8path(rr[0].path) == root / "New" / "FS" / "b.wav");
        CHECK(rr[1].status == LookupStatus::Missing);
        CHECK(rr[2].status == LookupStatus::Found);
        CHECK(rr[3].status == LookupStatus::Missing);
    }
    CHECK(calls == 4);
    // LocateFolder through the cache: no length callback on an unchanged rescan.
    calls = 0;
    LookupResult l = LocateFolder(Id("A.wav", 10, 1.0), (root / "New" / "FS").u8string(), lister);
    CHECK(l.status == LookupStatus::Found);
    CHECK(calls == 0);

    const std::string folder = (root / "New" / "FS").u8string();
    const std::string cp = FolderCachePath(res, folder);
    const std::string before = ReadAll(fs::u8path(cp));
    std::vector<DbFile> cached;
    CHECK(LoadFolderCache(cp, &cached) && cached.size() == 4);
    size_t n = 99;
    auto again = ScanFolder(folder, cached, length_of, &n);
    CHECK(n == 0 && again.size() == 4);
    CHECK(FolderCacheText(again) == before);
    CHECK(ReadAll(fs::u8path(cp)) == before);
    WriteAll(root / "New" / "FS" / "b.wav", std::string(21, 'b'));
    again = ScanFolder(folder, cached, length_of, &n);
    CHECK(n == 1);
    // Same size, new mtime: the length is computed again.
    const fs::path bp = root / "New" / "FS" / "b.wav";
    cached = again;
    WriteAll(bp, std::string(21, 'B'));
    fs::last_write_time(bp, fs::last_write_time(bp) + std::chrono::hours(2));
    again = ScanFolder(folder, cached, length_of, &n);
    CHECK(n == 1);
    // An unknown cached length (-1) is retried.
    for (auto& x : again) x.length = -1;
    again = ScanFolder(folder, again, length_of, &n);
    CHECK(n == 4);
    // > 4 GB size round trip through the cache grammar
    std::vector<DbFile> big = {F("C:/x/big.wav", 5000000000LL, 2.5)}, back;
    CHECK(SaveFolderCache((root / "big.ReaperFileList").u8string(), big));
    CHECK(LoadFolderCache((root / "big.ReaperFileList").u8string(), &back) && back.size() == 1);
    if (back.size() == 1) CHECK(back[0].size == 5000000000LL && back[0].length == 2.5);
    std::error_code ec;
    fs::remove_all(root, ec);
}

static void TestLoad()
{
    const fs::path res = TempDir("load");
    fs::create_directories(res / "MediaDB");
    for (const char* n : {"00.ReaperFileList", "01.ReaperFileList", "02.ReaperFileList"})
        fs::copy_file(fs::u8path(kData) / "MediaDB" / n, res / "MediaDB" / n);
    WriteAll(res / "MediaDB" / "||LastSearch||.ReaperFileList", "FILE \"C:\\last\\lastsearch.wav\" 1 0 0 0\n");

    auto wait = [] {
        for (int i = 0; i < 1000 && !MediaDbReady(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return MediaDbReady();
    };
    for (int pass = 0; pass < 2; ++pass) {
        // pass 0: listed in reaper.ini; pass 1: none listed -> MediaDB folder scan
        WriteAll(res / "reaper.ini", pass == 0 ? ReadAll(kData + "/reaper_explorer.ini") : "[reaper]\n");
        const auto t0 = std::chrono::steady_clock::now();
        MediaDbStartLoad(res.u8string());
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  StartLoad returned in %.3f ms\n", ms);
        CHECK(ms < 100);
        CHECK(wait());
        auto idx = MediaDbGet();
        CHECK(idx && idx->Size() == 9);
        CHECK(idx->Search("lastsearch", "", 10).empty());
        MediaDbInfo info = MediaDbGetInfo();
        CHECK(info.databases.size() == 3 && info.unparsed == 3 && info.files == 9);
        if (pass == 1 && info.databases.size() == 3) CHECK(info.databases[0].name == "00");
    }
    // A restart keeps the previous index visible until the new one is ready.
    MediaDbStartLoad(res.u8string());
    auto during = MediaDbGet();
    CHECK(during && during->Size() == 9);
    CHECK(wait());
    MediaDbStop();
    std::error_code ec;
    fs::remove_all(res, ec);
}

static void TestFavourites()
{
    auto v = LoadMediaFavourites("  C:\\A\\x.wav \nc:/a/X.wav\n\nD:\\Folder\n");
    CHECK(v.size() == 2 && v[0] == "C:\\A\\x.wav" && v[1] == "D:\\Folder");
    CHECK(SaveMediaFavourites(v) == "C:\\A\\x.wav\nD:\\Folder\n");
    const fs::path res = TempDir("fav");
    CHECK(WriteMediaFavouritesFile(res.u8string(), v));
    CHECK(ReadMediaFavouritesFile(res.u8string()) == v);
    CHECK(fs::exists(res / "ReaAnimViewer" / "media_favourites.txt"));
    std::error_code ec;
    fs::remove_all(res, ec);
}

static void Bench()
{
    MediaDbIndex idx;
    const int N = 405000;
    char name[64];
    for (int i = 0; i < N; ++i) {
        std::snprintf(name, sizeof(name), "D:/Bank/F%03d/snd_%d.wav", i % 500, i % 150000);
        DbFile f = F(name, 1000 + i, 0.5 + (i % 1000) * 0.01);
        f.description = (i % 7) ? "footstep dirt" : "sword whoosh";
        idx.Add(f);
    }
    auto t0 = std::chrono::steady_clock::now();
    int found = 0;
    for (int k = 0; k < 1000; ++k) {
        const int i = (k * 397) % N;
        std::snprintf(name, sizeof(name), "SND_%d.WAV", i % 150000);
        if (idx.Find(Id(name, 1000 + i, 0.5 + (i % 1000) * 0.01)).status == LookupStatus::Found) ++found;
    }
    const double avg = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 1000;
    t0 = std::chrono::steady_clock::now();
    const size_t hits = idx.Search("sword whoosh", "", 1000000).size();
    const double sms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  bench: %d entries, Find avg %.4f ms (%d/1000 found), Search %.1f ms (%zu hits)\n", N, avg, found,
                sms, hits);
    CHECK(avg < 1.0);
    CHECK(found == 1000);
}

int main(int argc, char** argv)
{
    if (argc > 1) {  // D9: real MediaDB folder (its parent = the resource dir)
        const fs::path db = fs::u8path(argv[1]);
        MediaDbStartLoad(db.parent_path().u8string());
        while (!MediaDbReady()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        MediaDbInfo info = MediaDbGetInfo();
        std::printf("real: %zu databases, %zu files, %zu unparsed, %.2f s\n", info.databases.size(), info.files,
                    info.unparsed, info.seconds);
        MediaDbStop();
        return 0;
    }
    TestLength();
    TestSpikeArithmetic();
    TestParseFixture();
    TestIni();
    TestFind();
    TestSearch();
    TestRelocateAndScan();
    TestLoad();
    TestFavourites();
    Bench();
    if (g_fail) {
        std::printf("media_db_test: %d FAILED\n", g_fail);
        return 1;
    }
    std::printf("media_db_test: all passed\n");
    return 0;
}
