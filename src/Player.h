#pragma once
#include <raylib.h>
#include <raymath.h>
#include "Common.h"

class Map;

class Player {
public:
    uint8_t id     = 0;
    bool    active = false;
    bool    alive  = true;
    int     health = MAX_HEALTH;

    Vector3 position = { 0, PLAYER_HEIGHT * 0.5f, 0 };
    float   yaw      = 0.0f;   // horizontal look angle in degrees
    float   pitch    = 0.0f;   // vertical look angle in degrees (clamped ±89)
    Vector3 velocity = { 0, 0, 0 };
    bool    onGround = false;

    // Camera eye is slightly above the center of the player capsule
    Vector3 eyePos() const;

    // World-space direction the player is looking
    Vector3 lookDir() const;

    // Set horizontal velocity from directional input (call each frame before update)
    void moveInput(bool forward, bool back, bool left, bool right, bool jump);

    // Advance physics one timestep and resolve collisions against the map
    void update(float dt, const Map& map);

    void takeDamage(int amount);
    void respawn(Vector3 spawnPos);
};
