#include "Server.h"
#include <cstring>
#include <cstdio>
#include <cmath>
#include <string>

// All #includes must be at file scope — never inside a function body.
#ifdef _WIN32
#  include <ws2tcpip.h>   // getaddrinfo, inet_ntop (already have winsock2 via enet)
#else
#  include <unistd.h>
#  include <ifaddrs.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#endif

// ---- Network info helpers ---------------------------------------------------

static std::string getLocalIP() {
#ifdef _WIN32
    char hostname[256] = {};
    if (gethostname(hostname, sizeof(hostname)) != 0) return "unknown";

    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hostname, nullptr, &hints, &res) != 0) return "unknown";

    char ipStr[INET_ADDRSTRLEN] = {};
    if (res && res->ai_addr) {
        auto* sin = reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
        inet_ntop(AF_INET, &sin->sin_addr, ipStr, sizeof(ipStr));
    }
    freeaddrinfo(res);
    return std::string(ipStr[0] ? ipStr : "unknown");

#else  // Linux / macOS
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) return "unknown";

    std::string result = "unknown";
    for (struct ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (std::string(ifa->ifa_name) == "lo") continue;
        char ip[INET_ADDRSTRLEN] = {};
        auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
        result = ip;
        break;
    }
    freeifaddrs(ifaddr);
    return result;
#endif
}

static std::string fetchPublicIP() {
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("api.ipify.org", "80", &hints, &res) != 0)
        return "(offline)";

#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return "unknown"; }
    DWORD tv = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    if (connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
        closesocket(s); freeaddrinfo(res); return "unknown";
    }
    freeaddrinfo(res);
    const char* req = "GET / HTTP/1.0\r\nHost: api.ipify.org\r\nConnection: close\r\n\r\n";
    send(s, req, static_cast<int>(strlen(req)), 0);
    char buf[512] = {};
    recv(s, buf, sizeof(buf) - 1, 0);
    closesocket(s);
#else
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) { freeaddrinfo(res); return "unknown"; }
    struct timeval tv = { 3, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(s, res->ai_addr, static_cast<socklen_t>(res->ai_addrlen)) != 0) {
        close(s); freeaddrinfo(res); return "unknown";
    }
    freeaddrinfo(res);
    const char* req = "GET / HTTP/1.0\r\nHost: api.ipify.org\r\nConnection: close\r\n\r\n";
    send(s, req, strlen(req), 0);
    char buf[512] = {};
    recv(s, buf, sizeof(buf) - 1, 0);
    close(s);
#endif

    const char* body = strstr(buf, "\r\n\r\n");
    if (!body) return "unknown";

    std::string ip(body + 4);
    while (!ip.empty() && (ip.back() == '\r' || ip.back() == '\n' || ip.back() == ' '))
        ip.pop_back();
    return ip;
}

static void printNetworkInfo() {
    printf("[Server] -----------------------------------------------\n");
    printf("[Server] LAN  IP (same network) : %s\n", getLocalIP().c_str());
    fflush(stdout);
    printf("[Server] WAN  IP (internet)     : %s\n", fetchPublicIP().c_str());
    printf("[Server] Port                   : %d  (UDP)\n", SERVER_PORT);
    printf("[Server] -----------------------------------------------\n");
    printf("[Server] Share your WAN IP for internet play (needs UDP %d forwarded).\n", SERVER_PORT);
    printf("[Server] Share your LAN IP for local network play.\n");
    printf("[Server] -----------------------------------------------\n");
}
// -----------------------------------------------------------------------------

static void platformSleep(int ms) {
#ifdef _WIN32
    Sleep(static_cast<DWORD>(ms));
#else
    usleep(ms * 1000);
#endif
}

// Spawn positions must use PLAYER_HEIGHT * 0.5f as Y so the player stands on
// the floor rather than being embedded in it or in a piece of cover.
static const float SPAWN_Y = PLAYER_HEIGHT * 0.5f;
static const Vector3 SPAWN_POINTS[MAX_PLAYERS] = {
    { -5, SPAWN_Y,  -5 }, {  5, SPAWN_Y,  -5 },
    { -5, SPAWN_Y,   5 }, {  5, SPAWN_Y,   5 },
    { -8, SPAWN_Y,   0 }, {  8, SPAWN_Y,   0 },
    {  0, SPAWN_Y,  -8 }, {  0, SPAWN_Y,   8 },
};

// ---- Lifecycle --------------------------------------------------------------

bool Server::init() {
    if (enet_initialize() != 0) {
        printf("[Server] ENet init failed\n");
        return false;
    }

    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = SERVER_PORT;

    m_host = enet_host_create(&addr, MAX_PLAYERS, NET_CHANNELS, 0, 0);
    if (!m_host) {
        printf("[Server] Could not bind to port %d\n", SERVER_PORT);
        return false;
    }

    m_peers.fill(nullptr);
    m_respawnTimers.fill(0.0f);
    printf("[Server] Listening on UDP port %d  (max %d players)\n", SERVER_PORT, MAX_PLAYERS);
    printNetworkInfo();
    return true;
}

void Server::run() {
    m_running = true;

    double lastTime = static_cast<double>(enet_time_get()) / 1000.0;

    while (m_running) {
        double now = static_cast<double>(enet_time_get()) / 1000.0;
        float  dt  = static_cast<float>(now - lastTime);
        lastTime   = now;
        if (dt > 0.1f) dt = 0.1f;  // clamp in case of lag spike

        // Drain all pending network events this tick
        ENetEvent event;
        while (enet_host_service(m_host, &event, 0) > 0) {
            switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                onConnect(event.peer);
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                onPacket(event.peer, event.packet);
                enet_packet_destroy(event.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                onDisconnect(event.peer);
                break;
            default: break;
            }
        }

        // Advance server-side physics for all active players
        for (auto& p : m_players)
            if (p.active && p.alive)
                p.update(dt, m_map);

        // Respawn dead players after a delay
        for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
            if (!m_players[i].active || m_players[i].alive) continue;
            m_respawnTimers[i] -= dt;
            if (m_respawnTimers[i] <= 0.0f) {
                m_players[i].respawn(SPAWN_POINTS[i]);
                printf("[Server] Player %d respawned\n", i);
            }
        }

        // Broadcast world state to all clients at ~20 Hz
        m_snapTimer += dt;
        if (m_snapTimer >= 0.05f) {
            broadcastSnapshot();
            m_snapTimer = 0.0f;
        }

        platformSleep(1);
    }
}

void Server::shutdown() {
    m_running = false;
    if (m_host) {
        enet_host_destroy(m_host);
        m_host = nullptr;
    }
    enet_deinitialize();
}

// ---- Connection events ------------------------------------------------------

uint8_t Server::findFreeSlot() const {
    for (uint8_t i = 0; i < MAX_PLAYERS; i++)
        if (!m_players[i].active) return i;
    return 255;
}

uint8_t Server::idFromPeer(ENetPeer* peer) const {
    for (uint8_t i = 0; i < MAX_PLAYERS; i++)
        if (m_peers[i] == peer) return i;
    return 255;
}

void Server::onConnect(ENetPeer* peer) {
    uint8_t slot = findFreeSlot();
    if (slot == 255) {
        printf("[Server] Server full, rejecting connection\n");
        enet_peer_disconnect(peer, 0);
        return;
    }

    m_peers[slot]      = peer;
    peer->data         = reinterpret_cast<void*>(static_cast<uintptr_t>(slot));

    Player& p          = m_players[slot];
    p.id               = slot;
    p.active           = true;
    p.respawn(SPAWN_POINTS[slot]);

    ConnectAckPacket ack;
    ack.assignedId = slot;
    sendReliable(peer, &ack, sizeof(ack));

    int activePlayers = 0;
    for (const auto& pl : m_players) if (pl.active) activePlayers++;
    printf("[Server] Player %d connected  (%d/%d)\n", slot, activePlayers, MAX_PLAYERS);
}

void Server::onDisconnect(ENetPeer* peer) {
    uint8_t id = idFromPeer(peer);
    if (id == 255) return;

    m_players[id].active = false;
    m_peers[id]          = nullptr;
    printf("[Server] Player %d disconnected\n", id);
}

// ---- Packet dispatch --------------------------------------------------------

void Server::onPacket(ENetPeer* peer, ENetPacket* pkt) {
    if (pkt->dataLength == 0) return;

    PacketType t = static_cast<PacketType>(pkt->data[0]);
    switch (t) {
    case PacketType::PLAYER_INPUT:
        if (pkt->dataLength >= sizeof(PlayerInputPacket)) {
            PlayerInputPacket p;
            memcpy(&p, pkt->data, sizeof(p));
            handlePlayerInput(p);
        }
        break;
    case PacketType::SHOOT:
        if (pkt->dataLength >= sizeof(ShootPacket)) {
            ShootPacket s;
            memcpy(&s, pkt->data, sizeof(s));
            handleShoot(s);
        }
        break;
    default: break;
    }
}

void Server::handlePlayerInput(const PlayerInputPacket& p) {
    if (p.id >= MAX_PLAYERS || !m_players[p.id].active) return;

    // Accept the client's reported position (simple authoritative model for now).
    // A more robust server would validate against max speed instead.
    m_players[p.id].position = { p.x, p.y, p.z };
    m_players[p.id].yaw      = p.yaw;
    m_players[p.id].pitch    = p.pitch;
}

void Server::handleShoot(const ShootPacket& s) {
    if (s.shooterId >= MAX_PLAYERS || !m_players[s.shooterId].active) return;

    Ray ray = {
        { s.ox, s.oy, s.oz },
        { s.dx, s.dy, s.dz }
    };

    uint8_t hitId    = 255;
    float   bestDist = GUN_RANGE;

    for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
        if (i == s.shooterId || !m_players[i].active || !m_players[i].alive) continue;

        Vector3 pos = m_players[i].position;
        BoundingBox box = {
            { pos.x - 0.4f, pos.y - PLAYER_HEIGHT * 0.5f, pos.z - 0.4f },
            { pos.x + 0.4f, pos.y + PLAYER_HEIGHT * 0.5f, pos.z + 0.4f }
        };

        RayCollision col = GetRayCollisionBox(ray, box);
        if (col.hit && col.distance < bestDist) {
            bestDist = col.distance;
            hitId    = i;
        }
    }

    // Compute where the bullet ends (hit point or max-range)
    float endDist = (hitId != 255) ? bestDist : GUN_RANGE;
    ShootEffectPacket fx;
    fx.shooterId = s.shooterId;
    fx.startX = s.ox; fx.startY = s.oy; fx.startZ = s.oz;
    fx.endX   = s.ox + s.dx * endDist;
    fx.endY   = s.oy + s.dy * endDist;
    fx.endZ   = s.oz + s.dz * endDist;
    broadcastUnreliable(&fx, sizeof(fx));   // visual only — ok to drop

    if (hitId == 255) return;

    int dmg = static_cast<int>(GUN_DAMAGE);
    m_players[hitId].takeDamage(dmg);

    HitEventPacket hit;
    hit.targetId  = hitId;
    hit.shooterId = s.shooterId;
    hit.newHealth = static_cast<int16_t>(m_players[hitId].health);
    broadcastReliable(&hit, sizeof(hit));

    printf("[Server] Player %d hit player %d for %d  (HP: %d)\n",
           s.shooterId, hitId, dmg, m_players[hitId].health);

    if (!m_players[hitId].alive) {
        m_respawnTimers[hitId] = 5.0f;   // 5-second respawn delay
        printf("[Server] Player %d was eliminated by player %d (respawn in 5s)\n",
               hitId, s.shooterId);
    }
}

// ---- Snapshot ---------------------------------------------------------------

void Server::broadcastSnapshot() {
    WorldSnapshotPacket snap;
    snap.playerCount = 0;

    for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
        if (!m_players[i].active) continue;
        PlayerSnapshot& s = snap.players[snap.playerCount++];
        s.id     = i;
        s.x      = m_players[i].position.x;
        s.y      = m_players[i].position.y;
        s.z      = m_players[i].position.z;
        s.yaw    = m_players[i].yaw;
        s.pitch  = m_players[i].pitch;
        s.health = static_cast<int16_t>(m_players[i].health);
        s.alive  = m_players[i].alive ? 1 : 0;
        s.ping   = m_peers[i] ? static_cast<uint16_t>(m_peers[i]->roundTripTime) : 0;
    }

    broadcastUnreliable(&snap, sizeof(snap));
}

// ---- Helpers ----------------------------------------------------------------

void Server::sendReliable(ENetPeer* peer, const void* data, size_t sz) {
    ENetPacket* pkt = enet_packet_create(data, sz, ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, pkt);
}

void Server::broadcastReliable(const void* data, size_t sz) {
    ENetPacket* pkt = enet_packet_create(data, sz, ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast(m_host, 0, pkt);
}

void Server::broadcastUnreliable(const void* data, size_t sz) {
    ENetPacket* pkt = enet_packet_create(data, sz, 0);
    enet_host_broadcast(m_host, 1, pkt);
}
