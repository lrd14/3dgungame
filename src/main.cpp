#include <cstring>
#include <cstdio>
#include "Server.h"
#include "Client.h"

static void printUsage(const char* exe) {
    printf("Usage:\n");
    printf("  %s --server                  Start a headless game server\n", exe);
    printf("  %s                           Connect to localhost\n", exe);
    printf("  %s --connect <ip>            Connect to a remote server\n", exe);
}

int main(int argc, char* argv[]) {
    bool        isServer  = false;
    const char* serverIp  = "127.0.0.1";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--server") == 0) {
            isServer = true;
        } else if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            serverIp = argv[++i];
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
    } else {
        printf("=== 3D Gun Game — Client (connecting to %s) ===\n", serverIp);
        Client client;
        if (!client.init(serverIp)) return 1;
        client.run();
        client.shutdown();
    }

    return 0;
}
