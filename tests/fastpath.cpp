// recomp_fastpath (src/runtime/fastpath.c): registration, dispatch by VA, fall-through to the lifted body when
// nobody handles a call, and the counters the session log reports.
extern "C" {
#include "../src/runtime/fastpath.c"
}
#include <cstdio>
#include <cstdlib>
static void require(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }

static int calls = 0;
static int take = 1;
static int handler() { ++calls; return take; }
static int other() { return 1; }

int main() {
    require(recomp_fastpath(0x005C97CBu) == 0, "nothing registered: the lifted body runs");
    require(nfs_fastpath_calls(0x005C97CBu) == 0, "an unregistered function is not counted");
    require(nfs_fastpath_register(0x005C97CBu, handler) == 1, "registered");
    require(recomp_fastpath(0x005C97CBu) == 1 && calls == 1, "the handler takes the call");
    take = 0;
    require(recomp_fastpath(0x005C97CBu) == 0 && calls == 2, "a handler that declines leaves the call to the game");
    require(nfs_fastpath_calls(0x005C97CBu) == 2 && nfs_fastpath_handled(0x005C97CBu) == 1, "counted: two calls, one handled");
    require(recomp_fastpath(0x005CDCA7u) == 0 && calls == 2, "another VA does not reach this handler");
    require(nfs_fastpath_register(0x005CDCA7u, other) == 1 && recomp_fastpath(0x005CDCA7u) == 1, "a second function, its own handler");
    require(nfs_fastpath_register(0x005C97CBu, handler) == 1 && nfs_fastpath_calls(0x005C97CBu) == 0, "registering again resets the counts");
    require(nfs_fastpath_register(0x005C97CBu, nullptr) == 1 && recomp_fastpath(0x005C97CBu) == 0 && calls == 2,
            "unregistered: the game's code again");
    require(nfs_fastpath_register(0x005CDCA7u, nullptr) == 1, "both gone");
    for (uint32_t i = 0; i < NFS_FASTPATH_SLOTS; ++i) require(nfs_fastpath_register(0x00500000u + i, other) == 1, "fills every slot");
    require(nfs_fastpath_register(0x00600000u, other) == 0, "no slot left: refused, nothing replaced");
    require(recomp_fastpath(0x00500000u + NFS_FASTPATH_SLOTS - 1) == 1 && recomp_fastpath(0x00600000u) == 0, "the slots still work");
    require(nfs_fastpath_register(0x00500003u, nullptr) == 1 && nfs_fastpath_register(0x00600000u, other) == 1, "a freed slot is reused");
    std::puts("fastpath: ok");
    return 0;
}
