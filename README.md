# Reconnect on Server Kick

Native TeamSpeak 3 client plugin for macOS that automatically reconnects the current tab to the same server after your own client is kicked from the server.

The plugin uses the TeamSpeak 3 Client Plugin SDK API 26, which is compatible with TeamSpeak 3 Client 3.6.0 or newer.

## Build

```sh
./scripts/fetch-sdk.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The fetch script downloads the required official TeamSpeak 3 Client Plugin SDK API 26 headers into `third_party/ts3client-pluginsdk`, which is ignored by git. To use a local SDK checkout instead:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTS3_PLUGIN_SDK_DIR=/path/to/ts3client-pluginsdk
cmake --build build
```

The macOS plugin binary is:

```text
build/reconnect_on_kick.dylib
```

On macOS, the default CMake configuration builds a universal `x86_64;arm64` dylib so it can load in either Intel/Rosetta or native Apple Silicon clients.

## Install on macOS

```sh
mkdir -p "$HOME/Library/Application Support/TeamSpeak 3/plugins"
cmake --install build
```

Restart TeamSpeak 3 Client and enable the plugin in the plugins/addons settings if it is not already enabled.

## Behavior

When TeamSpeak reports that your own client was kicked from the server, the plugin stores the current server address, server password when available, nickname, and channel path/password when available. It reconnects the same TeamSpeak tab after the tab reaches the disconnected state.

The plugin intentionally does not reconnect after bans, normal disconnects, server shutdowns, connection loss, or kicks affecting other clients.

## Package

To create a `.ts3_plugin` zip package:

```sh
cmake --build build --target package-ts3-plugin
```

The package is written to:

```text
build/reconnect_on_kick.ts3_plugin
```

It contains the TeamSpeak package layout:

```text
package.ini
plugins/reconnect_on_kick.dylib
```
