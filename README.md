# VocalStack for OBS Studio

Transcribe your OBS stream live with [VocalStack](https://vocalstack.com/obs-studio-transcription-plugin/), and
see the captions over OBS's preview. Captions never go into your stream or recordings.

- A **VocalStack dock** in OBS: sign in with one click (or an API key), pick a broadcast, set the spoken
  language and choose captions as spoken or translated.
- **Start transcribing** from the dock or a hotkey, or let it start and stop with OBS's own streaming or
  recording.
- The plugin sends OBS's audio (or audio and video) to VocalStack over its own connection, so the stream
  your viewers see is untouched. Transcriptions are saved in your VocalStack account.

## Download

Get the installer for your system from the [latest release](https://github.com/vocalstack/obs-plugin/releases/latest):

| System | File |
|---|---|
| Windows 10/11 (x64) | [vocalstack-obs-windows-x64.zip](https://github.com/vocalstack/obs-plugin/releases/latest/download/vocalstack-obs-windows-x64.zip) |
| macOS 12+ (Apple silicon and Intel) | [vocalstack-obs-macos.pkg](https://github.com/vocalstack/obs-plugin/releases/latest/download/vocalstack-obs-macos.pkg) |
| Ubuntu 24.04+ (x86_64) | [vocalstack-obs-linux-x86_64.deb](https://github.com/vocalstack/obs-plugin/releases/latest/download/vocalstack-obs-linux-x86_64.deb) |

Needs OBS Studio 31 or later.

- **Windows:** extract the zip into `C:\ProgramData\obs-studio\plugins`, so the folder
  `C:\ProgramData\obs-studio\plugins\vocalstack-obs` exists, and restart OBS.
- **macOS:** open the `.pkg`. It is not signed by Apple yet: if macOS refuses to open it, open
  **System Settings → Privacy & Security** and click **Open Anyway**. Restart OBS.
- **Linux:** `sudo apt install ./vocalstack-obs-linux-x86_64.deb`, then restart OBS.

Then open **Docks → VocalStack** if the dock is not showing, and click **Sign in with VocalStack**.

## Build

Built from [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate); GitHub Actions builds every
push for Windows, macOS and Linux, and a version tag (`1.2.3`) publishes a release.

```sh
# macOS (Homebrew's xattr lacks -r, which the template's configure needs)
PATH=/usr/bin:$PATH cmake --preset macos
PATH=/usr/bin:$PATH cmake --build build_macos --config RelWithDebInfo
cp -R build_macos/RelWithDebInfo/vocalstack-obs.plugin \
  ~/Library/Application\ Support/obs-studio/plugins/
```

`VOCALSTACK_API` and `VOCALSTACK_APP` point the plugin at another VocalStack environment.

## Licence

GPLv2 or later, as OBS Studio. See [LICENSE](LICENSE).
