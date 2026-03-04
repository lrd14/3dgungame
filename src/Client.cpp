#include "Client.h"
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cstdlib>    // rand()
#include <algorithm>
#include <rlgl.h>

// ---- Embedded lighting shaders ----------------------------------------------
//
// KEY: Raylib's rlgl pre-transforms vertices to world space inside rlVertex3f
// (it applies the rlPushMatrix/rlTranslatef stack at submission time, not at
// draw time).  The mvp uniform therefore only contains projection × view.
// This means vertexPosition in the shader IS already in world space, so we
// can do a real distance-attenuated point light without any extra uniforms.

static const char* LIGHTING_VS = R"glsl(
#version 330
layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 2) in vec3 vertexNormal;
layout(location = 3) in vec4 vertexColor;

uniform mat4 mvp;

out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragNormal;
out vec3 fragWorldPos;   // world-space position (rlgl gives us this for free)

void main() {
    fragTexCoord = vertexTexCoord;
    fragColor    = vertexColor;
    fragNormal   = vertexNormal;
    fragWorldPos = vertexPosition;   // already world space — see comment above
    gl_Position  = mvp * vec4(vertexPosition, 1.0);
}
)glsl";

static const char* LIGHTING_FS = R"glsl(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragNormal;
in vec3 fragWorldPos;

uniform sampler2D texture0;
uniform vec4      colDiffuse;
uniform vec3      lightDir;         // direction TO the sun (world space, normalised)
uniform vec3      lightColor;       // sun colour
uniform vec3      ambientColor;     // sky/fill colour

// Dynamic muzzle-flash point light.  Set colour to (0,0,0) when inactive.
uniform vec3      pointLightPos;    // world-space position of the gun muzzle
uniform vec3      pointLightColor;  // colour × intensity (pre-multiplied)

out vec4 finalColor;

void main() {
    vec4 texel  = texture(texture0, fragTexCoord);
    vec3 albedo = texel.rgb * fragColor.rgb * colDiffuse.rgb;

    vec3  N    = normalize(fragNormal);

    // Directional sun
    float diff = max(dot(N, normalize(lightDir)), 0.0);
    vec3  lit  = ambientColor * albedo + lightColor * diff * albedo;

    // Point light (muzzle flash) — quadratic falloff, 8-unit radius
    vec3  toLight = pointLightPos - fragWorldPos;
    float dist    = length(toLight);
    float atten   = max(0.0, 1.0 - dist / 8.0);
    atten         = atten * atten;
    float pdiff   = max(dot(N, normalize(toLight)), 0.0);
    lit += pointLightColor * pdiff * atten * albedo;

    finalColor = vec4(lit, texel.a * fragColor.a * colDiffuse.a);
}
)glsl";

// ---- Particle helpers -------------------------------------------------------

static float frand(float lo, float hi) {
    return lo + (hi - lo) * (static_cast<float>(rand()) / static_cast<float>(RAND_MAX));
}

static Color lerpColor(Color a, Color b, float t) {
    return {
        (unsigned char)(a.r + (b.r - a.r) * t),
        (unsigned char)(a.g + (b.g - a.g) * t),
        (unsigned char)(a.b + (b.b - a.b) * t),
        (unsigned char)(a.a + (b.a - a.a) * t)
    };
}

// ---- Lifecycle --------------------------------------------------------------

bool Client::initWindow() {
    if (enet_initialize() != 0) {
        printf("[Client] ENet init failed\n");
        return false;
    }

    m_host = enet_host_create(nullptr, 1, NET_CHANNELS, 0, 0);
    if (!m_host) {
        printf("[Client] Failed to create ENet host\n");
        return false;
    }

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(1280, 720, "3D Gun Game");

    int monitor  = GetCurrentMonitor();
    SetWindowSize(GetMonitorWidth(monitor), GetMonitorHeight(monitor));
    ToggleBorderlessWindowed();
    SetTextureFilter(GetFontDefault().texture, TEXTURE_FILTER_BILINEAR);
    SetTargetFPS(GetMonitorRefreshRate(monitor));
    EnableCursor();   // cursor visible in the menu

    m_camera.up         = { 0.0f, 1.0f, 0.0f };
    m_camera.fovy       = 90.0f;
    m_camera.projection = CAMERA_PERSPECTIVE;

    m_lightShader        = LoadShaderFromMemory(LIGHTING_VS, LIGHTING_FS);
    m_lightDirLoc        = GetShaderLocation(m_lightShader, "lightDir");
    m_lightColorLoc      = GetShaderLocation(m_lightShader, "lightColor");
    m_ambientLoc         = GetShaderLocation(m_lightShader, "ambientColor");
    m_pointLightPosLoc   = GetShaderLocation(m_lightShader, "pointLightPos");
    m_pointLightColorLoc = GetShaderLocation(m_lightShader, "pointLightColor");

    Vector3 sunDir   = Vector3Normalize({ 0.55f, 1.0f, 0.35f });
    Vector3 sunColor = { 0.85f, 0.80f, 0.68f };
    Vector3 ambient  = { 0.18f, 0.22f, 0.32f };
    SetShaderValue(m_lightShader, m_lightDirLoc,   &sunDir,   SHADER_UNIFORM_VEC3);
    SetShaderValue(m_lightShader, m_lightColorLoc, &sunColor, SHADER_UNIFORM_VEC3);
    SetShaderValue(m_lightShader, m_ambientLoc,    &ambient,  SHADER_UNIFORM_VEC3);

    Vector3 zero = {};
    SetShaderValue(m_lightShader, m_pointLightPosLoc,   &zero, SHADER_UNIFORM_VEC3);
    SetShaderValue(m_lightShader, m_pointLightColorLoc, &zero, SHADER_UNIFORM_VEC3);

    return true;
}

bool Client::connectToServer(const char* hostAndPort) {
    // Reset any leftover peer from a previous failed attempt
    if (m_peer) { enet_peer_reset(m_peer); m_peer = nullptr; }

    // Parse optional "hostname:port" — look for the last colon.
    // If the text after it is all digits we treat it as the port number.
    std::string hostStr = hostAndPort;
    uint16_t    port    = SERVER_PORT;

    size_t lastColon = hostStr.rfind(':');
    if (lastColon != std::string::npos) {
        std::string portPart = hostStr.substr(lastColon + 1);
        bool allDigits = !portPart.empty();
        for (char c : portPart) if (c < '0' || c > '9') { allDigits = false; break; }
        if (allDigits) {
            port    = static_cast<uint16_t>(std::stoi(portPart));
            hostStr = hostStr.substr(0, lastColon);
        }
    }

    ENetAddress addr;
    enet_address_set_host(&addr, hostStr.c_str());
    addr.port = port;

    m_peer = enet_host_connect(m_host, &addr, NET_CHANNELS, 0);
    if (!m_peer) return false;

    ENetEvent event;
    if (enet_host_service(m_host, &event, 5000) > 0 &&
        event.type == ENET_EVENT_TYPE_CONNECT) {
        printf("[Client] Connected to %s:%d\n", hostStr.c_str(), port);
        m_connected = true;
        return true;
    }

    printf("[Client] Could not connect to %s:%d\n", hostStr.c_str(), port);
    enet_peer_reset(m_peer);
    m_peer = nullptr;
    return false;
}

bool Client::init(const char* serverIp) {
    if (!initWindow()) return false;
    if (!connectToServer(serverIp)) return false;
    m_appState = AppState::PLAYING;
    DisableCursor();
    return true;
}

void Client::run() {
    if (m_appState != AppState::PLAYING)
        runMenu();

    // Game loop (skipped if user quit from the menu)
    m_running = true;
    while (m_running && !WindowShouldClose()) {
        float dt = GetFrameTime();
        processNetwork();
        processInput(dt);
        render();
    }
}

void Client::shutdown() {
    if (m_peer && m_connected) {
        enet_peer_disconnect(m_peer, 0);
        ENetEvent e;
        enet_host_service(m_host, &e, 1000);
    }
    if (m_host) {
        enet_host_destroy(m_host);
        m_host = nullptr;
    }
    enet_deinitialize();
    UnloadShader(m_lightShader);
    CloseWindow();
}

// ---- Main menu --------------------------------------------------------------

void Client::runMenu() {
    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        m_menuBlink += dt;

        // --- Text input ---
        int ch;
        while ((ch = GetCharPressed()) != 0) {
            if (ch >= 32 && ch < 127 && m_menuIp.size() < 64)
                m_menuIp += static_cast<char>(ch);
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !m_menuIp.empty())
            m_menuIp.pop_back();

        // Ctrl+V  — paste from clipboard, keeping only printable ASCII
        bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        if (ctrl && IsKeyPressed(KEY_V)) {
            const char* clip = GetClipboardText();
            if (clip) {
                for (const char* p = clip; *p && m_menuIp.size() < 64; ++p)
                    if (*p >= 32 && *p < 127)
                        m_menuIp += *p;
            }
        }

        // Ctrl+A  — select all (clears the field so the next keypress replaces it)
        if (ctrl && IsKeyPressed(KEY_A))
            m_menuIp.clear();

        // --- Connect on Enter or button click ---
        bool tryConnect = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);

        // Button click detection (rough rect, matches renderMenu below)
        const int W  = GetScreenWidth();
        const int H  = GetScreenHeight();
        const int BW = 220, BH = 50;
        const int BX = (W - BW) / 2, BY = H / 2 + 50;
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            Vector2 mp = GetMousePosition();
            if (mp.x >= BX && mp.x <= BX + BW && mp.y >= BY && mp.y <= BY + BH)
                tryConnect = true;
        }

        if (tryConnect && !m_menuIp.empty()) {
            m_menuStatus   = "Connecting...";
            m_menuStatusOk = true;
            renderMenu(dt);          // show status before blocking call

            if (connectToServer(m_menuIp.c_str())) {
                m_appState = AppState::PLAYING;
                DisableCursor();
                return;
            } else {
                m_menuStatus   = "Could not connect. Check the IP and try again.";
                m_menuStatusOk = false;
            }
        }

        renderMenu(dt);
    }
}

void Client::renderMenu(float dt) {
    const int W  = GetScreenWidth();
    const int H  = GetScreenHeight();

    BeginDrawing();
    ClearBackground({ 12, 12, 20, 255 });   // very dark navy

    // --- Background subtle grid ---
    for (int x = 0; x < W; x += 60)
        DrawLine(x, 0, x, H, { 255, 255, 255, 8 });
    for (int y = 0; y < H; y += 60)
        DrawLine(0, y, W, y, { 255, 255, 255, 8 });

    // --- Title ---
    const char* title = "3D GUN GAME";
    int titleSize = 72;
    int titleW    = MeasureText(title, titleSize);
    DrawText(title, (W - titleW) / 2, H / 2 - 180, titleSize, { 220, 80, 60, 255 });

    const char* sub = "MULTIPLAYER FPS";
    int subW = MeasureText(sub, 22);
    DrawText(sub, (W - subW) / 2, H / 2 - 100, 22, { 160, 160, 180, 200 });

    // --- Input card ---
    const int cardW = 460, cardH = 230;
    const int cardX = (W - cardW) / 2, cardY = H / 2 - 50;
    DrawRectangle(cardX, cardY, cardW, cardH, { 25, 25, 40, 230 });
    DrawRectangleLines(cardX, cardY, cardW, cardH, { 60, 60, 90, 255 });

    // Label
    DrawText("Server Address  (host:port)", cardX + 20, cardY + 20, 18, { 180, 180, 200, 255 });

    // Input field
    const int fieldX = cardX + 20, fieldY = cardY + 50;
    const int fieldW = cardW - 40, fieldH = 46;
    DrawRectangle(fieldX, fieldY, fieldW, fieldH, { 15, 15, 28, 255 });
    DrawRectangleLines(fieldX, fieldY, fieldW, fieldH, { 100, 100, 160, 255 });

    std::string displayIp = m_menuIp;
    bool showCursor = (fmodf(m_menuBlink, 1.0f) < 0.5f);
    if (showCursor) displayIp += '|';

    DrawText(displayIp.c_str(), fieldX + 12, fieldY + 13, 20, { 220, 230, 255, 255 });

    // Hint
    DrawText("e.g.  192.168.1.5:7777  or  abc.ply.gg:62308",
             cardX + 20, cardY + 108, 15, { 120, 120, 140, 200 });

    // Connect button
    const int BW = 220, BH = 50;
    const int BX = (W - BW) / 2, BY = cardY + cardH - 70;

    Vector2 mp       = GetMousePosition();
    bool    hovered  = mp.x >= BX && mp.x <= BX + BW && mp.y >= BY && mp.y <= BY + BH;
    Color   btnFill  = hovered ? Color{ 200, 60, 45, 255 } : Color{ 150, 45, 35, 255 };
    DrawRectangle(BX, BY, BW, BH, btnFill);
    DrawRectangleLines(BX, BY, BW, BH, { 240, 100, 80, 255 });
    const char* btnLabel = "CONNECT  [ Enter ]";
    int lblW = MeasureText(btnLabel, 18);
    DrawText(btnLabel, BX + (BW - lblW) / 2, BY + 16, 18, WHITE);

    // Status message
    if (!m_menuStatus.empty()) {
        Color sc = m_menuStatusOk ? Color{ 80, 220, 120, 255 }
                                  : Color{ 240, 90, 80, 255 };
        int sw = MeasureText(m_menuStatus.c_str(), 17);
        DrawText(m_menuStatus.c_str(), (W - sw) / 2, cardY + cardH + 16, 17, sc);
    }

    // ESC to quit hint
    DrawText("ESC  Quit", W - 110, H - 30, 15, { 80, 80, 100, 180 });

    EndDrawing();
}

// ---- Per-frame input --------------------------------------------------------

void Client::processInput(float dt) {
    if (m_myId == 255 || !m_players[m_myId].active || !m_posInitialized) return;

    Player& me = m_players[m_myId];

    // Mouse look
    Vector2 delta = GetMouseDelta();
    me.yaw   += delta.x * m_sensitivity;
    me.pitch -= delta.y * m_sensitivity;
    me.pitch  = Clamp(me.pitch, -89.0f, 89.0f);

    updateViewModel(dt, delta);

    // Movement keys — dt is required by the Source acceleration / friction model
    me.moveInput(
        IsKeyDown(KEY_W),
        IsKeyDown(KEY_S),
        IsKeyDown(KEY_A),
        IsKeyDown(KEY_D),
        IsKeyDown(KEY_SPACE),
        dt
    );

    // Snapshot ground state BEFORE physics so we can detect transitions
    bool wasOnGround = me.onGround;

    // Run client-side physics so movement feels instant (client prediction)
    me.update(dt, m_map);

    // Jump / land particle effects
    Vector3 feetPos = { me.position.x,
                        me.position.y - PLAYER_HEIGHT * 0.5f,
                        me.position.z };
    if (wasOnGround && !me.onGround) {
        // Just left the ground (jumped)
        emitJumpDust(feetPos);
    } else if (!wasOnGround && me.onGround) {
        // Just landed — scale dust by how hard the landing was
        float fallSpeed = fabsf(me.velocity.y);
        emitLandDust(feetPos, fallSpeed);
    }

    // Fire on left click
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && me.alive)
        sendShoot();

    // Send position to server ~60 times per second
    m_inputSendTimer += dt;
    if (m_inputSendTimer >= 1.0f / 60.0f) {
        sendInput();
        m_inputSendTimer = 0.0f;
    }

    // Hit flash countdown
    if (m_hitFlash) {
        m_hitFlashTimer -= dt;
        if (m_hitFlashTimer <= 0.0f) m_hitFlash = false;
    }

    // Particle simulation
    updateParticles(dt);

    // Age and remove expired tracers
    for (auto& t : m_tracers) t.life -= dt;
    m_tracers.erase(
        std::remove_if(m_tracers.begin(), m_tracers.end(),
                       [](const BulletTracer& t) { return t.life <= 0.0f; }),
        m_tracers.end());
}

// ---- Per-frame networking ---------------------------------------------------

void Client::processNetwork() {
    if (!m_connected) return;

    ENetEvent event;
    while (enet_host_service(m_host, &event, 0) > 0) {
        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            ENetPacket* pkt = event.packet;
            if (pkt->dataLength > 0) {
                PacketType t = static_cast<PacketType>(pkt->data[0]);
                switch (t) {
                case PacketType::CONNECT_ACK:
                    if (pkt->dataLength >= sizeof(ConnectAckPacket)) {
                        ConnectAckPacket p;
                        memcpy(&p, pkt->data, sizeof(p));
                        onConnectAck(p);
                    }
                    break;
                case PacketType::WORLD_SNAPSHOT:
                    if (pkt->dataLength >= sizeof(WorldSnapshotPacket)) {
                        WorldSnapshotPacket p;
                        memcpy(&p, pkt->data, sizeof(p));
                        onWorldSnapshot(p);
                    }
                    break;
                case PacketType::HIT_EVENT:
                    if (pkt->dataLength >= sizeof(HitEventPacket)) {
                        HitEventPacket p;
                        memcpy(&p, pkt->data, sizeof(p));
                        onHitEvent(p);
                    }
                    break;
                case PacketType::SHOOT_EFFECT:
                    if (pkt->dataLength >= sizeof(ShootEffectPacket)) {
                        ShootEffectPacket p;
                        memcpy(&p, pkt->data, sizeof(p));
                        onShootEffect(p);
                    }
                    break;
                default: break;
                }
            }
            enet_packet_destroy(pkt);
        } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            printf("[Client] Disconnected from server\n");
            m_connected = false;
        }
    }
}

// ---- Rendering --------------------------------------------------------------

void Client::render() {
    // Update camera to follow the local player's eye position
    if (m_myId < MAX_PLAYERS && m_players[m_myId].active) {
        Player& me        = m_players[m_myId];
        m_camera.position = me.eyePos();
        m_camera.target   = Vector3Add(me.eyePos(), me.lookDir());
    }

    BeginDrawing();
    ClearBackground(SKYBLUE);

    BeginMode3D(m_camera);
    renderWorld();
    renderViewModel();
    EndMode3D();

    renderHUD();
    EndDrawing();
}

void Client::renderWorld() {
    // --- Update muzzle-flash point light -------------------------------------
    float frameDt = GetFrameTime();
    m_muzzleLightTimer -= frameDt;
    if (m_muzzleLightTimer < 0.0f) m_muzzleLightTimer = 0.0f;

    {
        // Smooth quadratic fade: bright orange at t=1, dark by t=0
        float t = m_muzzleLightTimer / 0.10f;   // normalise to [0,1]
        t = t * t;                               // ease-out (starts bright, fades fast)
        Vector3 plColor = { 2.5f * t, 1.6f * t, 0.4f * t };   // warm orange-white
        SetShaderValue(m_lightShader, m_pointLightPosLoc,
                       &m_muzzleLightPos, SHADER_UNIFORM_VEC3);
        SetShaderValue(m_lightShader, m_pointLightColorLoc,
                       &plColor, SHADER_UNIFORM_VEC3);
    }

    // === Lit geometry (map + player capsules) =================================
    // BeginShaderMode works on all rlgl immediate-mode calls (DrawCube,
    // DrawSphere, DrawCylinder, DrawPlane) because they all emit vertices via
    // rlBegin/rlNormal3f/rlVertex3f.
    BeginShaderMode(m_lightShader);

    m_map.draw();

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == m_myId || !m_players[i].active || !m_players[i].alive) continue;

        const Player& p = m_players[i];
        Vector3 feet    = { p.position.x,
                            p.position.y - PLAYER_HEIGHT * 0.5f,
                            p.position.z };
        DrawCylinder(feet, 0.35f, 0.35f, PLAYER_HEIGHT * 0.85f, 8, RED);

        Vector3 headPos = { p.position.x,
                            p.position.y + PLAYER_HEIGHT * 0.45f,
                            p.position.z };
        DrawSphere(headPos, 0.22f, ORANGE);
    }

    EndShaderMode();
    // =========================================================================

    // --- Blob shadows (alpha blended, no lighting) ----------------------------
    BeginBlendMode(BLEND_ALPHA);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!m_players[i].active || !m_players[i].alive) continue;

        const Player& p        = m_players[i];
        float heightAboveFloor = p.position.y - PLAYER_HEIGHT * 0.5f;

        float radius = 0.42f + heightAboveFloor * 0.08f;
        int   alpha  = (int)(130.0f - heightAboveFloor * 25.0f);
        if (alpha < 10) alpha = 10;

        Vector3 shadowCenter = { p.position.x, 0.02f, p.position.z };
        DrawCircle3D(shadowCenter, radius, { 1, 0, 0 }, 90.0f,
                     { 0, 0, 0, (unsigned char)alpha });
    }
    EndBlendMode();

    // --- Bullet tracers (alpha blended, no lighting) --------------------------
    BeginBlendMode(BLEND_ALPHA);
    for (const auto& t : m_tracers) {
        float frac = t.life / t.maxLife;

        unsigned char coreA = (unsigned char)(frac * 230.0f);
        DrawLine3D(t.start, t.end, { 255, 240, 160, coreA });

        unsigned char haloA = (unsigned char)(frac * 100.0f);
        Color halo = { 255, 160, 40, haloA };
        Vector3 off1 = { t.start.x, t.start.y + 0.015f, t.start.z };
        Vector3 off2 = { t.end.x,   t.end.y   + 0.015f, t.end.z   };
        DrawLine3D(off1, off2, halo);
        Vector3 off3 = { t.start.x + 0.015f, t.start.y, t.start.z };
        Vector3 off4 = { t.end.x   + 0.015f, t.end.y,   t.end.z   };
        DrawLine3D(off3, off4, halo);
    }
    EndBlendMode();

    // --- Particles ------------------------------------------------------------
    renderParticles();
}

void Client::renderHUD() {
    const int W  = GetScreenWidth();
    const int H  = GetScreenHeight();
    const int cx = W / 2;
    const int cy = H / 2;

    // --- Dynamic crosshair ---------------------------------------------------
    // Project the muzzle's aim point (where the bullet will actually land) into
    // screen space so the crosshair sits exactly over the impact point even
    // though the barrel is offset from the camera eye.
    int crossX = cx, crossY = cy;   // fallback to screen centre

    if (m_myId < MAX_PLAYERS &&
        m_players[m_myId].active &&
        m_players[m_myId].alive &&
        m_posInitialized)
    {
        Vector3 muzzle = muzzlePos();
        Vector3 dir    = m_players[m_myId].lookDir();
        Ray     ray    = { muzzle, dir };

        float closestDist = GUN_RANGE;

        // Check map walls
        for (const auto& box : m_map.collisionBoxes()) {
            RayCollision col = GetRayCollisionBox(ray, box);
            if (col.hit && col.distance < closestDist)
                closestDist = col.distance;
        }

        // Check the floor plane (y = 0) — not in the collision box list
        if (dir.y < -0.0001f) {
            float t = -muzzle.y / dir.y;
            if (t > 0.0f && t < closestDist)
                closestDist = t;
        }

        // Project the aim point into screen space
        Vector3 aimPt  = Vector3Add(muzzle, Vector3Scale(dir, closestDist));
        Vector2 screen = GetWorldToScreen(aimPt, m_camera);

        // Only use the projected position when it's on-screen
        if (screen.x > 0 && screen.x < (float)W &&
            screen.y > 0 && screen.y < (float)H)
        {
            crossX = (int)screen.x;
            crossY = (int)screen.y;
        }
    }

    // Draw crosshair at projected aim point
    const int crossSize = 10;
    const int gap       = 4;    // centre gap so the dot is visible

    DrawLine(crossX - crossSize, crossY, crossX - gap, crossY, WHITE);
    DrawLine(crossX + gap,       crossY, crossX + crossSize, crossY, WHITE);
    DrawLine(crossX, crossY - crossSize, crossX, crossY - gap, WHITE);
    DrawLine(crossX, crossY + gap,       crossX, crossY + crossSize, WHITE);
    DrawCircleLines(crossX, crossY, 2, WHITE);  // centre dot

    // Damage flash: semi-transparent red vignette
    if (m_hitFlash)
        DrawRectangle(0, 0, W, H, { 220, 30, 30, 70 });

    // Health bar
    if (m_myId < MAX_PLAYERS && m_players[m_myId].active) {
        int hp = m_players[m_myId].health;
        DrawRectangle(10, H - 34, 200, 22, { 20, 20, 20, 180 });
        Color barColor = hp > 50 ? GREEN : (hp > 25 ? YELLOW : RED);
        DrawRectangle(10, H - 34, hp * 2, 22, barColor);
        DrawText(TextFormat("HP  %d", hp), 15, H - 32, 16, WHITE);
    }

    // Kill / death counter
    DrawText(TextFormat("K %d   D %d", m_kills, m_deaths), 10, 10, 20, WHITE);

    // Ping (use ENet's live RTT for our own peer — more accurate than snapshot)
    if (m_connected && m_peer) {
        int    ping      = static_cast<int>(m_peer->roundTripTime);
        Color  pingColor = ping < 50 ? GREEN : (ping < 100 ? YELLOW : RED);
        DrawText(TextFormat("Ping %d ms", ping), W - 130, 10, 16, pingColor);
    }

    // FPS (debug info)
    DrawText(TextFormat("FPS %d", GetFPS()), W - 130, 30, 16, LIME);

    // Speedometer — horizontal speed as a % of walk cap, with raw value below.
    // White = at or under walk speed, yellow = 110-200 %, red = 200 %+.
    if (m_myId < MAX_PLAYERS && m_players[m_myId].active && m_players[m_myId].alive) {
        const Vector3& vel = m_players[m_myId].velocity;
        float hspeed   = sqrtf(vel.x * vel.x + vel.z * vel.z);
        float pct      = hspeed / PLAYER_SPEED * 100.0f;

        Color spdColor = pct < 110.0f ? WHITE
                       : pct < 200.0f ? YELLOW : RED;

        // Main readout: percentage of walk speed
        const char* pctText = TextFormat("%.0f%%", pct);
        int tw1 = MeasureText(pctText, 22);
        DrawText(pctText, (W - tw1) / 2, H - 72, 22, spdColor);

        // Sub-label
        const char* subText = "of walk speed";
        int tw2 = MeasureText(subText, 14);
        DrawText(subText, (W - tw2) / 2, H - 48, 14, { 200, 200, 200, 180 });
    }

    // Tab scoreboard
    if (IsKeyDown(KEY_TAB)) {
        // Count active players for panel sizing
        int activePlayers = 0;
        for (int i = 0; i < MAX_PLAYERS; i++)
            if (m_players[i].active) activePlayers++;

        const int rowH   = 28;
        const int panelW = 420;
        const int panelH = 60 + activePlayers * rowH + 10;
        const int px     = (W - panelW) / 2;
        const int py     = (H - panelH) / 2;

        DrawRectangle(px, py, panelW, panelH, { 0, 0, 0, 200 });
        DrawRectangleLines(px, py, panelW, panelH, GRAY);

        DrawText("SCOREBOARD", px + panelW / 2 - 60, py + 10, 20, WHITE);
        DrawLine(px + 10, py + 38, px + panelW - 10, py + 38, GRAY);

        // Column headers
        DrawText("Player",  px + 14,       py + 42, 15, LIGHTGRAY);
        DrawText("HP",      px + 190,      py + 42, 15, LIGHTGRAY);
        DrawText("K / D",   px + 250,      py + 42, 15, LIGHTGRAY);
        DrawText("Ping",    px + 360,      py + 42, 15, LIGHTGRAY);
        DrawLine(px + 10, py + 59, px + panelW - 10, py + 59, DARKGRAY);

        int row = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!m_players[i].active) continue;

            int   ry        = py + 62 + row * rowH;
            bool  isMe      = (i == m_myId);
            Color nameColor = isMe ? YELLOW : WHITE;

            DrawText(TextFormat("Player %d%s", i, isMe ? " (you)" : ""),
                     px + 14, ry, 16, nameColor);

            int hp = m_players[i].health;
            DrawText(TextFormat("%d", hp), px + 190, ry, 16,
                     hp > 50 ? GREEN : (hp > 25 ? YELLOW : RED));

            // Only track our own K/D; others show placeholders
            if (isMe)
                DrawText(TextFormat("%d / %d", m_kills, m_deaths), px + 250, ry, 16, WHITE);
            else
                DrawText("- / -", px + 250, ry, 16, DARKGRAY);

            // Own ping comes from the live peer RTT; others from the snapshot
            int ping = isMe ? static_cast<int>(m_peer->roundTripTime)
                             : static_cast<int>(m_players[i].ping);
            Color pingColor = ping < 50 ? GREEN : (ping < 100 ? YELLOW : RED);
            DrawText(TextFormat("%d ms", ping), px + 360, ry, 16, pingColor);

            row++;
        }

        DrawText("[TAB] Scoreboard", cx - 65, H - 24, 14, DARKGRAY);
    } else {
        DrawText("[TAB] Scoreboard", cx - 65, H - 24, 14, { 120, 120, 120, 140 });
    }

    // Death screen
    if (m_myId < MAX_PLAYERS && m_players[m_myId].active && !m_players[m_myId].alive) {
        DrawRectangle(0, 0, W, H, { 0, 0, 0, 120 });
        DrawText("YOU DIED", cx - 80, cy - 30, 40, RED);
        DrawText("Respawning...", cx - 70, cy + 20, 22, LIGHTGRAY);
    }

    if (!m_connected)
        DrawText("DISCONNECTED", cx - 90, cy - 20, 24, RED);
}

// ---- Viewmodel --------------------------------------------------------------

// Shared helper: computes the gun's position and orientation matrix each frame.
// Both renderViewModel() and muzzlePos() call this so they're always in sync.
static void computeGunTransform(
    Vector3 eye, Vector3 lookFwd,
    float recoilZ, float swayX, float swayY, float bobY,
    Vector3& outPos, Vector3& outRight, Vector3& outUp, Vector3& outFwd)
{
    outFwd   = lookFwd;
    outRight = Vector3Normalize(Vector3CrossProduct(outFwd, { 0, 1, 0 }));
    if (Vector3Length(outRight) < 0.001f) outRight = { 1, 0, 0 };
    outUp    = Vector3CrossProduct(outRight, outFwd);

    outPos = Vector3Add(eye,
        Vector3Add(
            Vector3Scale(outFwd,  0.36f + recoilZ),
            Vector3Add(
                Vector3Scale(outRight, 0.18f + swayX),
                Vector3Scale(outUp,   -0.13f + swayY + bobY))));
}

void Client::updateViewModel(float dt, Vector2 mouseDelta) {
    // Recoil spring: kick drives m_recoilZ negative, spring brings it back
    float springForce = -28.0f * m_recoilZ - 7.0f * m_recoilZVel;
    m_recoilZVel += springForce * dt;
    m_recoilZ    += m_recoilZVel * dt;
    m_recoilZ     = Clamp(m_recoilZ, -0.13f, 0.01f);

    // Mouse sway — gun position lags slightly behind camera rotation
    float targetX  = Clamp(-mouseDelta.x * 0.0013f, -0.04f, 0.04f);
    float targetY  = Clamp(-mouseDelta.y * 0.0013f, -0.03f, 0.03f);
    float lerpRate = Clamp(dt * 14.0f, 0.0f, 1.0f);
    m_vmSwayX += (targetX - m_vmSwayX) * lerpRate;
    m_vmSwayY += (targetY - m_vmSwayY) * lerpRate;

    // Walk bob — accumulate phase while moving on the ground
    if (m_myId < MAX_PLAYERS && m_players[m_myId].alive) {
        const Player& me = m_players[m_myId];
        float hSpeed = sqrtf(me.velocity.x * me.velocity.x +
                             me.velocity.z * me.velocity.z);
        if (hSpeed > 0.5f && me.onGround)
            m_bobTime += dt * hSpeed * 1.5f;
    }
}

Vector3 Client::muzzlePos() const {
    if (m_myId >= MAX_PLAYERS || !m_players[m_myId].active)
        return { 0, 0, 0 };
    const Player& me = m_players[m_myId];
    float bob = sinf(m_bobTime) * 0.006f;

    Vector3 pos, right, up, fwd;
    computeGunTransform(me.eyePos(), me.lookDir(),
                        m_recoilZ, m_vmSwayX, m_vmSwayY, bob,
                        pos, right, up, fwd);

    // Barrel tip in gun-local space is ~{0, 0.013, 0.26}
    return Vector3Add(pos,
        Vector3Add(Vector3Scale(fwd, 0.26f),
                   Vector3Scale(up,  0.013f)));
}

void Client::renderViewModel() {
    if (m_myId >= MAX_PLAYERS ||
        !m_players[m_myId].active ||
        !m_players[m_myId].alive) return;

    const Player& me = m_players[m_myId];
    float bob = sinf(m_bobTime) * 0.006f;

    Vector3 pos, right, up, fwd;
    computeGunTransform(me.eyePos(), me.lookDir(),
                        m_recoilZ, m_vmSwayX, m_vmSwayY, bob,
                        pos, right, up, fwd);

    // Build column-major transform matrix for rlgl
    float mat[16] = {
        right.x, right.y, right.z, 0,
        up.x,    up.y,    up.z,    0,
        fwd.x,   fwd.y,   fwd.z,   0,
        pos.x,   pos.y,   pos.z,   1
    };

    // Disable depth test so the gun always renders in front of walls
    rlDisableDepthTest();
    rlPushMatrix();
    rlMultMatrixf(mat);

    // All coordinates below are in gun-local space:
    //   +Z = forward (muzzle direction)   +Y = up   +X = right

    // Main body / frame
    DrawCube({ 0,       0,        0      }, 0.050f, 0.082f, 0.220f, { 50, 50, 55, 255 });
    // Slide (top, slightly raised, slightly shorter than body)
    DrawCube({ 0,       0.036f,   0.010f }, 0.038f, 0.028f, 0.200f, { 68, 68, 72, 255 });
    // Barrel (extends forward past the slide)
    DrawCube({ 0,       0.013f,   0.190f }, 0.020f, 0.020f, 0.150f, { 38, 38, 43, 255 });
    // Grip
    DrawCube({ 0,      -0.087f,  -0.038f }, 0.040f, 0.100f, 0.055f, { 48, 48, 53, 255 });
    // Trigger guard
    DrawCubeWires({ 0, -0.025f,   0.030f }, 0.008f, 0.024f, 0.070f, { 30, 30, 35, 255 });
    // Front sight post
    DrawCube({ 0,       0.054f,   0.215f }, 0.005f, 0.009f, 0.007f, { 72, 72, 76, 255 });
    // Rear sight (two notch blocks)
    DrawCube({ -0.008f, 0.052f,  -0.085f }, 0.006f, 0.007f, 0.005f, { 72, 72, 76, 255 });
    DrawCube({  0.008f, 0.052f,  -0.085f }, 0.006f, 0.007f, 0.005f, { 72, 72, 76, 255 });

    rlPopMatrix();
    rlEnableDepthTest();
}

// ---- Outgoing packets -------------------------------------------------------

void Client::sendInput() {
    if (m_myId == 255) return;
    const Player& me = m_players[m_myId];

    PlayerInputPacket p;
    p.id    = m_myId;
    p.x     = me.position.x;
    p.y     = me.position.y;
    p.z     = me.position.z;
    p.yaw   = me.yaw;
    p.pitch = me.pitch;
    sendUnreliable(&p, sizeof(p));
}

void Client::sendShoot() {
    if (m_myId == 255) return;
    const Player& me = m_players[m_myId];

    // Kick the recoil spring
    m_recoilZVel = -0.20f;

    Vector3 origin = muzzlePos();
    Vector3 dir    = me.lookDir();

    // Arm the muzzle-flash point light (shader picks it up next renderWorld)
    constexpr float MUZZLE_LIGHT_DURATION = 0.10f;
    m_muzzleLightTimer = MUZZLE_LIGHT_DURATION;
    m_muzzleLightPos   = origin;

    // Immediate visual feedback: muzzle flash particles
    emitMuzzleFlash(origin, dir);

    ShootPacket s;
    s.shooterId = m_myId;
    s.ox = origin.x; s.oy = origin.y; s.oz = origin.z;
    s.dx = dir.x;    s.dy = dir.y;    s.dz = dir.z;
    sendReliable(&s, sizeof(s));
}

// ---- Incoming packet handlers -----------------------------------------------

void Client::onConnectAck(const ConnectAckPacket& p) {
    m_myId = p.assignedId;
    m_players[m_myId].id     = m_myId;
    m_players[m_myId].active = true;
    printf("[Client] Assigned player ID: %d\n", m_myId);
}

void Client::onWorldSnapshot(const WorldSnapshotPacket& snap) {
    for (int i = 0; i < snap.playerCount; i++) {
        const PlayerSnapshot& s = snap.players[i];
        if (s.id >= MAX_PLAYERS) continue;

        if (s.id == m_myId) {
            bool wasAlive = m_players[m_myId].alive;
            m_players[m_myId].health = s.health;
            m_players[m_myId].alive  = (s.alive != 0);

            bool justSpawned = !m_posInitialized ||
                               (!wasAlive && m_players[m_myId].alive);
            if (justSpawned) {
                m_players[m_myId].position  = { s.x, s.y, s.z };
                m_players[m_myId].velocity  = { 0, 0, 0 };
                m_players[m_myId].onGround  = true;
                m_posInitialized = true;
            }
        } else {
            Player& p   = m_players[s.id];
            p.id        = s.id;
            p.active    = true;
            p.position  = { s.x, s.y, s.z };
            p.yaw       = s.yaw;
            p.pitch     = s.pitch;
            p.health    = s.health;
            p.alive     = (s.alive != 0);
            p.ping      = s.ping;
        }
    }
}

void Client::onShootEffect(const ShootEffectPacket& p) {
    constexpr float TRACER_LIFE = 0.18f;   // seconds — similar to CS:GO
    BulletTracer t;
    t.start   = { p.startX, p.startY, p.startZ };
    t.end     = { p.endX,   p.endY,   p.endZ   };
    t.life    = TRACER_LIFE;
    t.maxLife = TRACER_LIFE;
    m_tracers.push_back(t);

    // Sparks at the bullet's impact point
    emitBulletImpact({ p.endX, p.endY, p.endZ });
}

void Client::onHitEvent(const HitEventPacket& p) {
    if (p.targetId < MAX_PLAYERS) {
        m_players[p.targetId].health = p.newHealth;
        m_players[p.targetId].alive  = (p.newHealth > 0);
    }

    // Local player was hit — show red flash
    if (p.targetId == m_myId) {
        m_hitFlash      = true;
        m_hitFlashTimer = 0.35f;
        if (p.newHealth <= 0) m_deaths++;
    }

    // Local player got a kill
    if (p.shooterId == m_myId && p.newHealth <= 0 && p.targetId != m_myId) {
        m_kills++;
        printf("[Client] You eliminated player %d  (K:%d D:%d)\n",
               p.targetId, m_kills, m_deaths);
    }
}

// ---- Send helpers -----------------------------------------------------------

void Client::sendReliable(const void* data, size_t sz) {
    if (!m_peer || !m_connected) return;
    ENetPacket* pkt = enet_packet_create(data, sz, ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(m_peer, 0, pkt);
}

void Client::sendUnreliable(const void* data, size_t sz) {
    if (!m_peer || !m_connected) return;
    ENetPacket* pkt = enet_packet_create(data, sz, 0);
    enet_peer_send(m_peer, 1, pkt);
}

// ---- Particle system --------------------------------------------------------

void Client::emitMuzzleFlash(Vector3 pos, Vector3 dir) {
    // A tight forward cone of bright orange-white sparks
    for (int i = 0; i < 10; i++) {
        // Cone spread: mostly forward, slight random XY
        Vector3 spread = {
            dir.x + frand(-0.25f, 0.25f),
            dir.y + frand(-0.25f, 0.25f),
            dir.z + frand(-0.25f, 0.25f)
        };
        spread = Vector3Normalize(spread);

        Particle p;
        p.pos      = pos;
        p.vel      = Vector3Scale(spread, frand(3.0f, 8.0f));
        p.life     = frand(0.04f, 0.10f);
        p.maxLife  = p.life;
        p.colStart = { 255, 220, 100, 255 };   // bright yellow-white
        p.colEnd   = { 255, 80,  20,  0   };   // fade to orange-transparent
        p.size     = frand(0.030f, 0.065f);
        p.additive = true;
        m_particles.push_back(p);
    }
    // A couple of larger, slower embers
    for (int i = 0; i < 4; i++) {
        Particle p;
        p.pos      = pos;
        p.vel      = { frand(-1.5f,1.5f), frand(0.5f,2.5f), frand(-1.5f,1.5f) };
        p.life     = frand(0.10f, 0.22f);
        p.maxLife  = p.life;
        p.colStart = { 255, 140, 30, 200 };
        p.colEnd   = { 100,  30,  0,  0  };
        p.size     = frand(0.04f, 0.08f);
        p.additive = true;
        m_particles.push_back(p);
    }
}

void Client::emitJumpDust(Vector3 feetPos) {
    for (int i = 0; i < 10; i++) {
        float angle = frand(0.0f, 6.2832f);
        float speed = frand(0.8f, 2.2f);
        Particle p;
        p.pos      = { feetPos.x + frand(-0.15f,0.15f),
                       feetPos.y + 0.05f,
                       feetPos.z + frand(-0.15f,0.15f) };
        p.vel      = { cosf(angle) * speed, frand(0.3f, 1.0f), sinf(angle) * speed };
        p.life     = frand(0.25f, 0.50f);
        p.maxLife  = p.life;
        p.colStart = { 180, 170, 150, 160 };
        p.colEnd   = { 150, 140, 120,   0 };
        p.size     = frand(0.05f, 0.11f);
        p.additive = false;
        m_particles.push_back(p);
    }
}

void Client::emitLandDust(Vector3 feetPos, float fallSpeed) {
    int count = (int)(8 + fallSpeed * 2.0f);
    if (count > 22) count = 22;
    for (int i = 0; i < count; i++) {
        float angle = frand(0.0f, 6.2832f);
        float speed = frand(1.2f, 3.5f) * (1.0f + fallSpeed * 0.15f);
        Particle p;
        p.pos      = { feetPos.x + frand(-0.2f,0.2f),
                       feetPos.y + 0.03f,
                       feetPos.z + frand(-0.2f,0.2f) };
        p.vel      = { cosf(angle) * speed, frand(0.2f, 1.2f), sinf(angle) * speed };
        p.life     = frand(0.35f, 0.70f);
        p.maxLife  = p.life;
        p.colStart = { 160, 148, 130, 200 };
        p.colEnd   = { 130, 120, 100,   0 };
        p.size     = frand(0.07f, 0.16f);
        p.additive = false;
        m_particles.push_back(p);
    }
}

void Client::emitBulletImpact(Vector3 pos) {
    for (int i = 0; i < 7; i++) {
        Particle p;
        p.pos      = pos;
        p.vel      = { frand(-4.0f,4.0f), frand(1.0f,5.0f), frand(-4.0f,4.0f) };
        p.life     = frand(0.08f, 0.20f);
        p.maxLife  = p.life;
        p.colStart = { 255, 240, 180, 255 };
        p.colEnd   = { 200, 100,  20,   0 };
        p.size     = frand(0.020f, 0.045f);
        p.additive = true;
        m_particles.push_back(p);
    }
    // Small concrete/dirt chips (alpha-blended)
    for (int i = 0; i < 5; i++) {
        Particle p;
        p.pos      = pos;
        p.vel      = { frand(-2.5f,2.5f), frand(0.5f,3.5f), frand(-2.5f,2.5f) };
        p.life     = frand(0.20f, 0.45f);
        p.maxLife  = p.life;
        p.colStart = { 160, 152, 140, 200 };
        p.colEnd   = { 120, 110, 100,   0 };
        p.size     = frand(0.04f, 0.08f);
        p.additive = false;
        m_particles.push_back(p);
    }
}

void Client::updateParticles(float dt) {
    constexpr float GRAV = -9.0f;   // softer than player gravity for floaty dust
    for (auto& p : m_particles) {
        p.life    -= dt;
        p.vel.y   += GRAV * dt;
        p.pos.x   += p.vel.x * dt;
        p.pos.y   += p.vel.y * dt;
        p.pos.z   += p.vel.z * dt;
        // Bounce off the floor
        if (p.pos.y < 0.02f) { p.pos.y = 0.02f; p.vel.y *= -0.25f; }
    }
    m_particles.erase(
        std::remove_if(m_particles.begin(), m_particles.end(),
                       [](const Particle& p) { return p.life <= 0.0f; }),
        m_particles.end());
}

void Client::renderParticles() {
    // Two passes: additive sparks first (order independent), then alpha dust.
    for (int pass = 0; pass < 2; pass++) {
        bool wantAdditive = (pass == 0);
        BeginBlendMode(wantAdditive ? BLEND_ADDITIVE : BLEND_ALPHA);
        for (const auto& p : m_particles) {
            if (p.additive != wantAdditive) continue;
            float t   = 1.0f - (p.life / p.maxLife);   // 0 = just spawned, 1 = dying
            Color col = lerpColor(p.colStart, p.colEnd, t);
            DrawCube(p.pos, p.size, p.size, p.size, col);
        }
        EndBlendMode();
    }
}
