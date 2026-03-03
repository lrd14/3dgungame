#pragma once

// See Server.h for the rationale behind these suppressors.
#ifdef _WIN32
#  define NOGDI
#  define NOUSER
#endif
#include <enet/enet.h>
#ifdef _WIN32
#  undef NOGDI
#  undef NOUSER
#  undef PlaySound
#endif

#include <raylib.h>
#include <array>
#include "Common.h"
#include "Player.h"
#include "Map.h"

class Client {
public:
    // Connect to the server at serverIp:SERVER_PORT and open the game window
    bool init(const char* serverIp);

    void run();
    void shutdown();

private:
    // --- Network ---
    ENetHost* m_host      = nullptr;
    ENetPeer* m_peer      = nullptr;
    uint8_t   m_myId      = 255;   // 255 = not yet assigned
    bool      m_connected = false;

    // --- Game state ---
    std::array<Player, MAX_PLAYERS> m_players{};
    Map      m_map;
    Camera3D m_camera{};

    // --- Timers ---
    float m_inputSendTimer = 0.0f;

    // --- HUD state ---
    int   m_kills           = 0;
    int   m_deaths          = 0;
    bool  m_hitFlash        = false;
    float m_hitFlashTimer   = 0.0f;

    float m_sensitivity    = 0.10f;
    bool  m_running        = false;
    bool  m_posInitialized = false;  // true once we've received our first spawn position

    // Per-frame processing
    void processNetwork();
    void processInput(float dt);
    void render();
    void renderWorld();
    void renderHUD();

    // Outgoing packets
    void sendInput();
    void sendShoot();

    // Incoming packet handlers
    void onConnectAck(const ConnectAckPacket& p);
    void onWorldSnapshot(const WorldSnapshotPacket& p);
    void onHitEvent(const HitEventPacket& p);

    void sendReliable(const void* data, size_t sz);
    void sendUnreliable(const void* data, size_t sz);
};
