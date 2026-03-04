#include "Player.h"
#include "Map.h"
#include <cmath>

// ---- Source / Quake movement physics ----------------------------------------
//
// Ground friction uses the Quake formula: bleeding horizontal speed based on a
// "control" value that prevents creeping near-zero velocities.
// Accelerate() is used for BOTH ground and air movement; the key insight is that
// it only adds velocity in the WISH direction up to `wishspeed` projected onto
// that direction — so strafing perpendicular to your current velocity can always
// add speed (air strafing), while moving straight never exceeds PLAYER_SPEED.

// Ground accel: takes ~0.25 s to reach full walk speed from a dead stop.
// (accelspeed per frame = SV_ACCELERATE * PLAYER_SPEED * dt;
//  frames to reach max  = PLAYER_SPEED / accelspeed_per_frame = 1 / (SV_ACCELERATE * dt))
static constexpr float SV_ACCELERATE    = 6.0f;
// Air accel: lower cap keeps the speed-gain-per-strafe manageable.
static constexpr float SV_AIRACCELERATE = 2.5f;
static constexpr float SV_FRICTION      = 5.5f;   // snappier stops
static constexpr float SV_STOPSPEED     = 2.0f;   // below this, friction zeroes vel

static void applyFriction(Vector3& vel, float dt) {
    float speed = sqrtf(vel.x * vel.x + vel.z * vel.z);
    if (speed < 0.001f) { vel.x = vel.z = 0.0f; return; }
    // Use stopspeed as minimum control value so we actually come to a full stop
    float control    = (speed < SV_STOPSPEED) ? SV_STOPSPEED : speed;
    float drop       = control * SV_FRICTION * dt;
    float newspeed   = speed - drop;
    if (newspeed < 0.0f) newspeed = 0.0f;
    float scale = newspeed / speed;
    vel.x *= scale;
    vel.z *= scale;
}

// wishdir must be unit length (or zero).
static void sourceAccelerate(Vector3& vel, Vector3 wishdir, float wishspeed,
                              float accel, float dt) {
    // How fast are we already moving in the wish direction?
    float currentspeed = vel.x * wishdir.x + vel.z * wishdir.z;
    // Only add velocity up to the remaining headroom
    float addspeed = wishspeed - currentspeed;
    if (addspeed <= 0.0f) return;
    float accelspeed = accel * wishspeed * dt;
    if (accelspeed > addspeed) accelspeed = addspeed;
    vel.x += accelspeed * wishdir.x;
    vel.z += accelspeed * wishdir.z;
}

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

void Player::moveInput(bool fwd, bool back, bool left, bool right, bool jump,
                       float dt) {
    float yRad = yaw * DEG2RAD;

    // Match the -Z forward convention from lookDir()
    Vector3 forward = {  sinf(yRad), 0.0f, -cosf(yRad) };
    Vector3 rightV  = {  cosf(yRad), 0.0f,  sinf(yRad) };

    Vector3 wish = { 0, 0, 0 };
    if (fwd)   wish = Vector3Add(wish, forward);
    if (back)  wish = Vector3Subtract(wish, forward);
    if (right) wish = Vector3Add(wish, rightV);
    if (left)  wish = Vector3Subtract(wish, rightV);

    float wishlen    = Vector3Length(wish);
    Vector3 wishdir  = (wishlen > 0.001f) ? Vector3Normalize(wish) : Vector3Zero();
    float wishspeed  = (wishlen > 0.001f) ? PLAYER_SPEED : 0.0f;

    if (onGround) {
        // Friction is applied before jump so a held-space bhop still bleeds a
        // tiny amount of speed per landing frame (authentic Source behaviour).
        applyFriction(velocity, dt);
        sourceAccelerate(velocity, wishdir, wishspeed, SV_ACCELERATE, dt);

        // Auto-bhop: holding SPACE causes an immediate re-jump every landing frame,
        // preserving most horizontal momentum (friction already ran above).
        if (jump) {
            velocity.y = JUMP_SPEED;
            onGround   = false;
        }
    } else {
        // Air movement: no friction, lower effective acceleration — but strafing
        // perpendicular to velocity always has full headroom, enabling speed gain.
        sourceAccelerate(velocity, wishdir, wishspeed, SV_AIRACCELERATE, dt);
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
