// SPDX-License-Identifier: MIT
//
// Placeholder host test of src/media_db.h (Epic 13, created by 13-2): it links the module and calls
// one stub. Story 13-5 replaces it with the real tests.

#include "media_db.h"

#include <cstdio>

using namespace rav;

int main()
{
    MediaDbIndex idx;
    (void)idx.Find(AssetIdentity{});  // STUB call
    std::printf("media_db_test: placeholder passed (13-2 stub)\n");
    return 0;
}
