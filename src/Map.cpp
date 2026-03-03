#include "Map.h"

void Map::draw() const {
    // Ground plane
    DrawPlane({ 0.0f, 0.0f, 0.0f }, { 40.0f, 40.0f }, DARKGREEN);

    for (const auto& b : m_boxes) {
        DrawCube(b.pos, b.size.x, b.size.y, b.size.z, b.color);
        DrawCubeWires(b.pos, b.size.x, b.size.y, b.size.z, BLACK);
    }
}

void Map::buildDefault() {
    const float wallH = 3.0f;

    // Outer boundary
    addBox({  0,  wallH * 0.5f, -20 }, { 40, wallH, 1 }, DARKGRAY);  // North
    addBox({  0,  wallH * 0.5f,  20 }, { 40, wallH, 1 }, DARKGRAY);  // South
    addBox({ -20, wallH * 0.5f,   0 }, {  1, wallH, 40 }, DARKGRAY); // West
    addBox({  20, wallH * 0.5f,   0 }, {  1, wallH, 40 }, DARKGRAY); // East

    // Cover objects (CS:GO-style boxes and barriers)
    addBox({ -6, 0.75f, -6 }, { 4, 1.5f, 1 }, GRAY);
    addBox({  6, 0.75f,  6 }, { 4, 1.5f, 1 }, GRAY);
    addBox({ -6, 0.75f,  6 }, { 1, 1.5f, 4 }, LIGHTGRAY);
    addBox({  6, 0.75f, -6 }, { 1, 1.5f, 4 }, LIGHTGRAY);
    addBox({  0, 0.75f,  0 }, { 3, 1.5f, 3 }, GRAY);
    addBox({ -10, 0.75f, 0 }, { 2, 1.5f, 5 }, BEIGE);
    addBox({  10, 0.75f, 0 }, { 2, 1.5f, 5 }, BEIGE);
}

void Map::addBox(Vector3 pos, Vector3 size, Color color) {
    m_boxes.push_back({ pos, size, color });
    m_colBoxes.push_back({
        { pos.x - size.x * 0.5f, pos.y - size.y * 0.5f, pos.z - size.z * 0.5f },
        { pos.x + size.x * 0.5f, pos.y + size.y * 0.5f, pos.z + size.z * 0.5f }
    });
}
