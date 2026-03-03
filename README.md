# 3D Gun Game

A from-scratch 3D multiplayer FPS in C++ using Raylib (rendering) and ENet (networking).

## Project structure

```
src/
  Common.h        -- Network packet definitions and gameplay constants
  Map.h / .cpp    -- Level geometry and collision boxes
  Player.h / .cpp -- Player state, movement, physics
  Server.h / .cpp -- Headless game server (ENet host, hit detection)
  Client.h / .cpp -- Game client (Raylib window, input, rendering)
  main.cpp        -- Entry point: --server or client mode
```

## Controls

| Key / Button   | Action         |
|----------------|----------------|
| W A S D        | Move           |
| Mouse          | Look           |
| Space          | Jump           |
| Left click     | Shoot          |
| Esc / close    | Quit           |

---

## Setup — Arch Linux

### 1 — Install dependencies

```bash
sudo pacman -S base-devel cmake raylib enet
```

### 2 — Build

```bash
git clone <your-repo-url>
cd 3dgungame/3dgungame
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### 3 — Run

```bash
# Terminal 1 — server
./build/3dgungame --server

# Terminal 2 — client (connects to localhost)
./build/3dgungame

# Connect to a remote server
./build/3dgungame --connect 192.168.1.42
```

---

## Setup — Windows (Visual Studio 2022)

### 1 — Install vcpkg

Open a terminal and run:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg integrate install
```

### 2 — Install dependencies

```powershell
C:\vcpkg\vcpkg install raylib:x64-windows enet:x64-windows
```

### 3 — Add source files to the Visual Studio project

1. Open `3dgungame.sln` in Visual Studio.
2. In **Solution Explorer**, right-click the `3dgungame` project → **Add → Existing Item**.
3. Select all `.cpp` and `.h` files inside the `src/` folder.

### 4 — Configure the project to use vcpkg

If `vcpkg integrate install` worked correctly, the headers and libraries are found
automatically. If you see "cannot open include file" errors, verify in
**Project Properties**:

- **C/C++ → General → Additional Include Directories**: add `C:\vcpkg\installed\x64-windows\include`
- **Linker → General → Additional Library Directories**: add `C:\vcpkg\installed\x64-windows\lib`
- **Linker → Input → Additional Dependencies**: add `raylib.lib;enet.lib;winmm.lib;ws2_32.lib`

### 5 — Build and run

**Start the server** (run from the VS output folder or set command arguments):

```
3dgungame.exe --server
```

**Start a client** (in a second terminal or as another VS debug instance):

```
3dgungame.exe
```

To connect to a remote machine:

```
3dgungame.exe --connect 192.168.1.42
```

---

## Architecture notes

### Client / Server model

The executable runs in two modes via the `--server` flag:

- **Server** — headless process. Owns authoritative game state. Runs physics for
  every player, validates all shots via ray-AABB intersection, and broadcasts a
  world snapshot to all clients 20 times per second.

- **Client** — opens a Raylib window. Runs **client-side prediction**: applies
  your own movement locally each frame so the game feels instant, then sends
  your position to the server 60 times per second. Receives snapshots and
  interpolates remote players.

### Networking (ENet)

ENet gives you two reliable concepts on top of UDP:

- **Channel 0** — reliable ordered (used for shots, hit events, connect ACK)
- **Channel 1** — unreliable unordered (used for frequent position snapshots)

### What to build next

1. **Respawn system** — timer-based respawn, score board
2. **Interpolation** — smooth remote player movement between snapshots
3. **Lag compensation** — rewind player positions on the server when processing shots
4. **Weapons** — separate weapon classes, ammo, reload animation
5. **Audio** — `InitAudioDevice()` + `PlaySound()` in Raylib
6. **Proper map** — load a `.obj` mesh with `LoadModel()` instead of debug boxes
7. **Vulkan renderer** — once the game logic is solid, you can replace the Raylib
   render calls with a custom Vulkan backend
