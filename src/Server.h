#pragma once

// ENet pulls in windows.h on Windows.  Suppress the GDI / User subsystems so
// their macros (Rectangle, CloseWindow, ShowCursor, DrawText, PlaySound …)
// don't collide with raylib's identifiers of the same name.
#ifdef _WIN32
#  define NOGDI
#  define NOUSER
#endif
#include <enet/enet.h>
#ifdef _WIN32
#  undef NOGDI
#  undef NOUSER
#  undef PlaySound   // mmsystem.h defines PlaySound -> PlaySoundW
#endif

#include <array>
#include "Common.h"
#include "Player.h"
#include "Map.h"

class Server {
public:
    bool init();
    void run();       // blocks until the server is stopped
    void shutdown();

private:
    ENetHost* m_host = nullptr;

    std::array<Player,    MAX_PLAYERS> m_players{};
    std::array<ENetPeer*, MAX_PLAYERS> m_peers{};
    std::array<float,     MAX_PLAYERS> m_respawnTimers{};

    Map   m_map;
    float m_snapTimer = 0.0f;
    bool  m_running   = false;

    // Returns the first inactive slot, or 255 if full
    uint8_t findFreeSlot() const;

    // Resolve a peer pointer back to a player ID
    uint8_t idFromPeer(ENetPeer* peer) const;

    void onConnect(ENetPeer* peer);
    void onDisconnect(ENetPeer* peer);
    void onPacket(ENetPeer* peer, ENetPacket* pkt);

    void handlePlayerInput(ENetPeer* peer, const PlayerInputPacket& p);
    void handleShoot(ENetPeer* peer, const ShootPacket& s);

    void broadcastSnapshot();

    void sendReliable(ENetPeer* peer, const void* data, size_t sz);
    void broadcastReliable(const void* data, size_t sz);
    void broadcastUnreliable(const void* data, size_t sz);
};
