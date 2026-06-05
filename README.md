# CrimsonDesertAnalogMovement

Native ASI plugin experiment for Crimson Desert analog movement.

Current state: minimum working movement plugin. It polls XInput and taps the
controller A button once when the left stick is pushed forward past `ForwardOn`,
then waits to re-arm until the stick drops below `ForwardOff`.

It does this by hooking the game's imported `XInputGetState` and briefly adding
`XINPUT_GAMEPAD_A` to the returned button state.

It logs beside the ASI at:

```text
CrimsonDesertAnalogMovement.log
```

## Build

Requires CMake and a Windows C++ compiler.

```powershell
cmake -S . -B build
cmake --build build --config Release
```

The built plugin should be:

```text
build\Release\CrimsonDesertAnalogMovement.asi
```

Copy the `.asi` and `CrimsonDesertAnalogMovement.ini` next to the game's ASI
loader target, usually the folder where the other Crimson Desert `.asi` mods
are currently installed.

For this machine, the detected target is:

```text
D:\Games\Steam\steamapps\common\Crimson Desert\bin64
```

After building, deploy with:

```powershell
.\scripts\deploy.ps1
```
