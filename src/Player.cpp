#include "Player.h"
#include "Map.h"
#include <cmath>

Vector3 Player::eyePos() const {
    // position.y is the capsule centre; feet are at position.y - PLAYER_HEIGHT*0.5.
    // Eye sits at 90% of total height above the feet.
    float feet = position.y - PLAYER_HEIGHT * 0.5f;
    return { position.x, feet + PLAYER_HEIGHT * 0.9f, position.z };
}

Vector3 Player::lookDir() const {
    float y = yaw   * DEG2RAD;
    float p = pitch * DEG2RAD;
    // Negative Z so yaw=0 faces into the screen (-Z) — the standard OpenGL
    // camera convention.  Without this, left/right are mirrored.
    return {
         cosf(p) * sinf(y),
         sinf(p),
        -cosf(p) * cosf(y)
    };
}

void Player::moveInput(bool fwd, bool back, bool left, bool right, bool jump) {
    float yRad = yaw * DEG2RAD;

    // Match the -Z forward convention from lookDir()
    Vector3 forward = {  sinf(yRad), 0.0f, -cosf(yRad) };
    Vector3 rightV  = {  cosf(yRad), 0.0f,  sinf(yRad) };

    Vector3 wish = { 0, 0, 0 };
    if (fwd)   wish = Vector3Add(wish, forward);
    if (back)  wish = Vector3Subtract(wish, forward);
    if (right) wish = Vector3Add(wish, rightV);
    if (left)  wish = Vector3Subtract(wish, rightV);

    float len = Vector3Length(wish);
    if (len > 0.01f) {
        wish = Vector3Scale(Vector3Normalize(wish), PLAYER_SPEED);
        velocity.x = wish.x;
        velocity.z = wish.z;
    } else {
        velocity.x = 0.0f;
        velocity.z = 0.0f;
    }

    if (jump && onGround) {
        velocity.y = JUMP_SPEED;
        onGround   = false;
    }
}

void Player::update(float dt, const Map& map) {
    if (!alive) return;

    if (!onGround) velocity.y += GRAVITY * dt;

    Vector3 next = Vector3Add(position, Vector3Scale(velocity, dt));

    // Floor
    const float floorY = PLAYER_HEIGHT * 0.5f;
    if (next.y <= floorY) {
        next.y     = floorY;
        velocity.y = 0.0f;
        onGround   = true;
    } else {
        onGround = false;
    }

    // Wall collision: try separating axes independently so sliding works
    auto makeBox = [](Vector3 p) -> BoundingBox {
        return {
            { p.x - 0.35f, p.y - PLAYER_HEIGHT * 0.5f, p.z - 0.35f },
            { p.x + 0.35f, p.y + PLAYER_HEIGHT * 0.5f, p.z + 0.35f }
        };
    };

    for (const auto& wall : map.collisionBoxes()) {
        BoundingBox pb = makeBox(next);
        if (!CheckCollisionBoxes(pb, wall)) continue;

        // Try cancelling Z movement first
        Vector3 tryXOnly = { next.x, next.y, position.z };
        if (!CheckCollisionBoxes(makeBox(tryXOnly), wall)) {
            next = tryXOnly;
            velocity.z = 0.0f;
            continue;
        }
        // Try cancelling X movement
        Vector3 tryZOnly = { position.x, next.y, next.z };
        if (!CheckCollisionBoxes(makeBox(tryZOnly), wall)) {
            next = tryZOnly;
            velocity.x = 0.0f;
            continue;
        }
        // Both axes collide — stop horizontal movement entirely
        next.x = position.x;
        next.z = position.z;
        velocity.x = velocity.z = 0.0f;
    }

    position = next;
}

void Player::takeDamage(int amount) {
    health -= amount;
    if (health <= 0) { health = 0; alive = false; }
}

void Player::respawn(Vector3 spawnPos) {
    position = spawnPos;
    velocity = { 0, 0, 0 };
    health   = MAX_HEALTH;
    alive    = true;
}
