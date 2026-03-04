#include <cstring>
#include <cstdio>
#include "Server.h"
#include "Client.h"

static void printUsage(const char* exe) {
    printf("Usage:\n");
    printf("  %s --server           Start a headless game server\n", exe);
    printf("  %s                    Open game with connect menu\n", exe);
    printf("  %s --connect <ip>     Skip menu and connect directly\n", exe);
}

int main(int argc, char* argv[]) {
    bool        isServer      = false;
    bool        hasConnectArg = false;
    const char* connectIp     = "127.0.0.1";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--server") == 0) {
            isServer = true;
        } else if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            hasConnectArg = true;
            connectIp     = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        }
    }

    if (isServer) {
        printf("=== 3D Gun Game — Server ===\n");
        Server server;
        if (!server.init()) return 1;
        server.run();
        server.shutdown();
        return 0;
    }

    Client client;

    if (hasConnectArg) {
        // Skip the menu — useful for testing
        printf("=== 3D Gun Game — Quick connect to %s ===\n", connectIp);
        if (!client.init(connectIp)) return 1;
    } else {
        // Open the window and show the connect menu
        if (!client.initWindow()) return 1;
    }

    client.run();
    client.shutdown();
    return 0;
}
