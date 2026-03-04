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
#include <vector>
#include <string>
#include "Common.h"
#include "Player.h"
#include "Map.h"

// A single bullet-tracer line that fades out over its lifetime
struct BulletTracer {
    Vector3 start;
    Vector3 end;
    float   life;
    float   maxLife;
};

// One CPU-simulated particle (small coloured cube, gravity-affected optional)
struct Particle {
    Vector3 pos;
    Vector3 vel;
    float   life;       // seconds remaining
    float   maxLife;
    Color   colStart;
    Color   colEnd;     // colour is linearly interpolated over lifetime
    float   size;
    bool    additive;   // true = BLEND_ADDITIVE (sparks), false = BLEND_ALPHA (dust)
};

enum class AppState { MENU, PLAYING };

class Client {
public:
    // Open the window and initialise everything except the network connection.
    // Call this when you want to show the main menu first.
    bool initWindow();

    // Convenience: initWindow() + connectToServer(ip) in one call.
    // Used by the --connect command-line shortcut.
    bool init(const char* serverIp);

    // If already connected (via init), runs the game loop directly.
    // Otherwise shows the main menu until the player connects, then plays.
    void run();

    void shutdown();

private:
    // --- App state ---
    AppState    m_appState   = AppState::MENU;

    // --- Menu state ---
    std::string m_menuIp     = "127.0.0.1:7777";  // pre-filled with localhost
    std::string m_menuStatus;                 // "" = idle, else shown below field
    bool        m_menuStatusOk = false;       // green vs red status colour
    float       m_menuBlink    = 0.0f;        // cursor blink accumulator

    // --- Network ---
    ENetHost* m_host      = nullptr;
    ENetPeer* m_peer      = nullptr;
    uint8_t   m_myId      = 255;   // 255 = not yet assigned
    bool      m_connected = false;

    // --- Game state ---
    std::array<Player, MAX_PLAYERS> m_players{};
    Map      m_map;
    Camera3D m_camera{};

    // Active bullet tracers (added on SHOOT_EFFECT, expire after ~0.18 s)
    std::vector<BulletTracer> m_tracers;

    // Particle pool
    std::vector<Particle> m_particles;

    // --- Lighting shader ---
    Shader  m_lightShader{};
    int     m_lightDirLoc        = -1;
    int     m_lightColorLoc      = -1;
    int     m_ambientLoc         = -1;
    int     m_pointLightPosLoc   = -1;
    int     m_pointLightColorLoc = -1;

    // Muzzle-flash point light state
    float   m_muzzleLightTimer = 0.0f;   // counts down from MUZZLE_LIGHT_DURATION
    Vector3 m_muzzleLightPos   = {};     // world-space position set on each shot

    // --- Timers ---
    float m_inputSendTimer = 0.0f;

    // --- HUD state ---
    int   m_kills           = 0;
    int   m_deaths          = 0;
    bool  m_hitFlash        = false;
    float m_hitFlashTimer   = 0.0f;

    float m_sensitivity    = 0.10f;
    bool  m_running        = false;
    bool  m_posInitialized = false;

    // --- Viewmodel animation state ---
    float m_recoilZ        = 0.0f;
    float m_recoilZVel     = 0.0f;
    float m_vmSwayX        = 0.0f;
    float m_vmSwayY        = 0.0f;
    float m_bobTime        = 0.0f;

    // Network helper called from menu and init()
    bool connectToServer(const char* ip);

    // Menu loop (called by run() when not yet connected)
    void runMenu();
    void renderMenu(float dt);

    // Per-frame processing (game)
    void processNetwork();
    void processInput(float dt);
    void render();
    void renderWorld();
    void renderHUD();
    void updateViewModel(float dt, Vector2 mouseDelta);
    void renderViewModel();

    // Particle emitters
    void emitMuzzleFlash(Vector3 pos, Vector3 dir);
    void emitJumpDust(Vector3 feetPos);
    void emitLandDust(Vector3 feetPos, float fallSpeed);
    void emitBulletImpact(Vector3 pos);
    void updateParticles(float dt);
    void renderParticles();

    // World-space position of the gun muzzle — used as the ray origin when shooting
    Vector3 muzzlePos() const;

    // Outgoing packets
    void sendInput();
    void sendShoot();

    // Incoming packet handlers
    void onConnectAck(const ConnectAckPacket& p);
    void onWorldSnapshot(const WorldSnapshotPacket& p);
    void onHitEvent(const HitEventPacket& p);
    void onShootEffect(const ShootEffectPacket& p);

    void sendReliable(const void* data, size_t sz);
    void sendUnreliable(const void* data, size_t sz);
};
