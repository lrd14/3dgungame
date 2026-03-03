#include "Client.h"
#include <cstring>
#include <cstdio>
#include <cmath>

// ---- Lifecycle --------------------------------------------------------------

bool Client::init(const char* serverIp) {
    if (enet_initialize() != 0) {
        printf("[Client] ENet init failed\n");
        return false;
    }

    m_host = enet_host_create(nullptr, 1, NET_CHANNELS, 0, 0);
    if (!m_host) {
        printf("[Client] Failed to create ENet host\n");
        return false;
    }

    ENetAddress addr;
    enet_address_set_host(&addr, serverIp);
    addr.port = SERVER_PORT;

    m_peer = enet_host_connect(m_host, &addr, NET_CHANNELS, 0);
    if (!m_peer) {
        printf("[Client] Connection initiation failed\n");
        return false;
    }

    // Block until the server accepts the connection (5 second timeout)
    ENetEvent event;
    if (enet_host_service(m_host, &event, 5000) > 0 &&
        event.type == ENET_EVENT_TYPE_CONNECT) {
        printf("[Client] Connected to %s:%d\n", serverIp, SERVER_PORT);
        m_connected = true;
    } else {
        printf("[Client] Could not connect to %s:%d\n", serverIp, SERVER_PORT);
        enet_peer_reset(m_peer);
        return false;
    }

    // Open the Raylib window
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(1280, 720, "3D Gun Game");
    SetTargetFPS(144);
    DisableCursor();  // lock + hide cursor for mouse-look

    m_camera.up         = { 0.0f, 1.0f, 0.0f };
    m_camera.fovy       = 90.0f;
    m_camera.projection = CAMERA_PERSPECTIVE;

    return true;
}

void Client::run() {
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
    CloseWindow();
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

    // Movement keys
    me.moveInput(
        IsKeyDown(KEY_W),
        IsKeyDown(KEY_S),
        IsKeyDown(KEY_A),
        IsKeyDown(KEY_D),
        IsKeyDown(KEY_SPACE)
    );

    // Run client-side physics so movement feels instant (client prediction)
    me.update(dt, m_map);

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
    EndMode3D();

    renderHUD();
    EndDrawing();
}

void Client::renderWorld() {
    m_map.draw();

    // Draw each remote player as a simple capsule (cylinder + sphere for head)
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == m_myId || !m_players[i].active || !m_players[i].alive) continue;

        const Player& p   = m_players[i];
            // Draw body from feet upward so the visual aligns with the hitbox
            Vector3 feet = { p.position.x,
                             p.position.y - PLAYER_HEIGHT * 0.5f,
                             p.position.z };
            DrawCylinder(feet, 0.35f, 0.35f, PLAYER_HEIGHT * 0.85f, 8, RED);

            // Head sphere sits on top of the body cylinder
            Vector3 headPos = { p.position.x,
                                p.position.y + PLAYER_HEIGHT * 0.45f,
                                p.position.z };
            DrawSphere(headPos, 0.22f, ORANGE);
    }
}

void Client::renderHUD() {
    const int W  = GetScreenWidth();
    const int H  = GetScreenHeight();
    const int cx = W / 2;
    const int cy = H / 2;

    // Crosshair
    const int crossSize = 10;
    DrawLine(cx - crossSize, cy, cx + crossSize, cy, WHITE);
    DrawLine(cx, cy - crossSize, cx, cy + crossSize, WHITE);
    DrawCircleLines(cx, cy, 2, WHITE);

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

    // FPS (debug info)
    DrawText(TextFormat("FPS %d", GetFPS()), W - 80, 10, 16, LIME);

    // Death screen
    if (m_myId < MAX_PLAYERS && m_players[m_myId].active && !m_players[m_myId].alive) {
        DrawRectangle(0, 0, W, H, { 0, 0, 0, 120 });
        DrawText("YOU DIED", cx - 80, cy - 30, 40, RED);
        DrawText("Respawning...", cx - 70, cy + 20, 22, LIGHTGRAY);
    }

    if (!m_connected)
        DrawText("DISCONNECTED", cx - 90, cy - 20, 24, RED);
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

    ShootPacket s;
    s.shooterId = m_myId;
    Vector3 eye = me.eyePos();
    Vector3 dir = me.lookDir();
    s.ox = eye.x; s.oy = eye.y; s.oz = eye.z;
    s.dx = dir.x; s.dy = dir.y; s.dz = dir.z;
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
        }
    }
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
