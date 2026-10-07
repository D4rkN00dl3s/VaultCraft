// Exercises the plugin's link and logging outside the game, so a fault here can be debugged in
// seconds instead of by relaunching Fallout: New Vegas.
//
//   vclink_test
//
// It creates the shared mapping, heartbeats a few times, then reports what it saw. Run it while no
// game is running: the mapping it creates is released when this process exits.
#include "PCH.h"
#include "Link.h"
#include "Log.h"

#include <cstdio>

int main()
{
    vaultcraft::log::Init();
    printf("VaultCraft link test\n");

    auto& link = vaultcraft::Link::Get();
    if (!link.Create()) {
        vaultcraft::log::Error("Link::Create failed");
        printf("FAIL: Link::Create returned false\n");
        return 1;
    }
    printf("Link::Create OK, valid=%d\n", link.Valid() ? 1 : 0);

    for (int i = 0; i < 5; ++i) {
        link.Heartbeat();
    }
    vaultcraft::log::Info("five heartbeats written");

    printf("McAlive=%d (expected 0 with no Minecraft)\n", link.McAlive() ? 1 : 0);
    printf("McPid=%u (expected 0)\n", link.McPid());
    vaultcraft::log::Info("link test finished");

    printf("PASS\n");
    return 0;
}