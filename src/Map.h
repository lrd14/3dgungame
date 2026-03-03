#pragma once
#include <vector>
#include <raylib.h>

struct MapBox {
    Vector3 pos;
    Vector3 size;
    Color   color;
};

// Simple axis-aligned box map.
// Both the server and client use this; only the client calls draw().
class Map {
public:
    Map() { buildDefault(); }

    // Render all geometry (call inside BeginMode3D / EndMode3D on client only)
    void draw() const;

    // AABB list used for player collision and server-side ray tests
    const std::vector<BoundingBox>& collisionBoxes() const { return m_colBoxes; }

private:
    void buildDefault();
    void addBox(Vector3 pos, Vector3 size, Color color);

    std::vector<MapBox>      m_boxes;
    std::vector<BoundingBox> m_colBoxes;
};
