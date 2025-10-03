Uploading the Web UI (HTML/CSS/JS) to ESP32 (M5Atom) with PlatformIO + LittleFS

This project serves the web UI (e.g., index.html, wifi.html, JS/CSS) from the ESP32’s flash using LittleFS. You’ll use VS Code + PlatformIO IDE to build and upload the filesystem image.

1) Install the tools

Visual Studio Code
Download and install VS Code for your OS.

PlatformIO IDE (VS Code extension)

Open VS Code → Extensions (Ctrl+Shift+X) → search “PlatformIO IDE” → Install.

After install, you’ll see the alien-head icon (left toolbar).

2) Open the project

VS Code → PlatformIO Home (alien icon) → Open Project → select this repository’s folder (where platformio.ini lives).

3) Build & upload the LittleFS image
Using the PlatformIO UI

In VS Code, click the alien icon → PIO Project Tasks → your env (m5stack-atom) → Platform:

Build Filesystem Image (bundles data/ into littlefs.bin)

Upload Filesystem Image (flashes it to the ESP32)