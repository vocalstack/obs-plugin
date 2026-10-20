# VocalStack for OBS

Sends OBS's audio (or audio and video) to VocalStack over RTMPS alongside the
main stream, starts and stops live transcriptions, and shows the captions over
OBS's preview only: never in the stream or recordings.

Work in progress, private until released.

## Build (macOS)

```sh
# Homebrew's xattr lacks -r, which the template's configure needs.
PATH=/usr/bin:$PATH cmake --preset macos
PATH=/usr/bin:$PATH cmake --build build_macos --config RelWithDebInfo
cp -R build_macos/RelWithDebInfo/vocalstack-obs.plugin \
  ~/Library/Application\ Support/obs-studio/plugins/
```

Built from [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate). GPLv2, as OBS.
