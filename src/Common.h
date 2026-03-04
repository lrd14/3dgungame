#pragma once
#include <cstdint>

// ENet on Windows requires Winsock and the multimedia timer library.
// These pragmas tell the linker to pull them in automatically.
#ifdef _WIN32
#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "winmm.lib")
#endif

// ---- Network ----------------------------------------------------------------
constexpr uint16_t SERVER_PORT  = 7777;
constexpr int      MAX_PLAYERS  = 8;
constexpr int      NET_CHANNELS = 2;   // channel 0 = reliable, channel 1 = unreliable

// ---- Gameplay ---------------------------------------------------------------
constexpr float PLAYER_SPEED  = 6.0f;
constexpr float PLAYER_HEIGHT = 1.75f;
constexpr float JUMP_SPEED    = 7.0f;
constexpr float GRAVITY       = -20.0f;
constexpr int   MAX_HEALTH    = 100;
constexpr float GUN_DAMAGE    = 26.0f;   // ~4 shots to kill
constexpr float GUN_RANGE     = 200.0f;

// ---- Packet type tag (first byte of every packet) ---------------------------
enum class PacketType : uint8_t {
    CONNECT_ACK = 0,   // server -> new client: you are player N
    PLAYER_INPUT,      // client -> server: my position + angles
    WORLD_SNAPSHOT,    // server -> all clients: everyone's state
    SHOOT,             // client -> server: I fired
    HIT_EVENT,         // server -> all clients: someone was hit
    SHOOT_EFFECT,      // server -> all clients: visual tracer for any shot
};

// Keep structs tightly packed so sizeof() matches what we send over the wire.
#pragma pack(push, 1)

struct ConnectAckPacket {
    PacketType type       = PacketType::CONNECT_ACK;
    uint8_t    assignedId = 0;
};

struct PlayerInputPacket {
    PacketType type  = PacketType::PLAYER_INPUT;
    uint8_t    id    = 0;
    float      x, y, z;
    float      yaw, pitch;
};

// Per-player data inside a world snapshot
struct PlayerSnapshot {
    uint8_t  id;
    float    x, y, z;
    float    yaw, pitch;
    int16_t  health;
    uint8_t  alive;   // 1 = alive, 0 = dead
    uint16_t ping;    // round-trip time in ms (from ENet, server-measured)
};

struct WorldSnapshotPacket {
    PacketType     type        = PacketType::WORLD_SNAPSHOT;
    uint8_t        playerCount = 0;
    PlayerSnapshot players[MAX_PLAYERS];
};

struct ShootPacket {
    PacketType type      = PacketType::SHOOT;
    uint8_t    shooterId = 0;
    float      ox, oy, oz;   // ray origin
    float      dx, dy, dz;   // ray direction (normalized)
};

struct HitEventPacket {
    PacketType type      = PacketType::HIT_EVENT;
    uint8_t    targetId  = 0;
    uint8_t    shooterId = 0;
    int16_t    newHealth = 0;
};

// Sent for every shot (hit or miss) so clients can draw bullet tracers
struct ShootEffectPacket {
    PacketType type      = PacketType::SHOOT_EFFECT;
    uint8_t    shooterId = 0;
    float      startX, startY, startZ;   // muzzle position
    float      endX,   endY,   endZ;     // impact point or max-range endpoint
};

#pragma pack(pop)
