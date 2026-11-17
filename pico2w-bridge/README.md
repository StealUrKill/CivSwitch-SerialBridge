# CivSwitch Serial Bridge — Pico 2 W

Native Raspberry Pi Pico SDK firmware for the Pico 2 W. It provides the open
`Switch2BridgePico` access point, DHCP, captive DNS and HTTP replies, the USB
serial commands `STATUS`, `SCAN`, and `SEND`, and TCP payload relay.

Install the Pico SDK and ARM GNU toolchain. Add the toolchain `bin` directory
to `PATH`. Set the SDK path, then configure and build the firmware:

```powershell
$env:PICO_SDK_PATH = Read-Host 'Path to the Pico SDK'
cmake -S . -B build -DPICO_SDK_PATH="$env:PICO_SDK_PATH" -DPICO_BOARD=pico2_w
cmake --build build
```

The build creates these UF2 files in `build/`:

- `build/civswitch_serial_bridge.uf2` — standard firmware.
- `build/civswitch_serial_bridge_diagnostic.uf2` — diagnostic firmware.

Git ignores `build/`, so it does not show these generated files as repository
changes. The repository root also contains ready-to-flash copies. To update
those copies after a build, run:

```powershell
Copy-Item build\civswitch_serial_bridge.uf2 .\civswitch-pico2w.uf2 -Force
Copy-Item build\civswitch_serial_bridge_diagnostic.uf2 .\civswitch-pico2w-diagnostic.uf2 -Force
```
