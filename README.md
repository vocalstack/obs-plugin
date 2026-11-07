# VocalStack for OBS Studio

[![Push](https://github.com/vocalstack/obs-plugin/actions/workflows/push.yaml/badge.svg)](https://github.com/vocalstack/obs-plugin/actions/workflows/push.yaml) [![Notarized](https://github.com/vocalstack/obs-plugin/actions/workflows/notarized.yml/badge.svg)](https://github.com/vocalstack/obs-plugin/actions/workflows/notarized.yml)

Transcribe your OBS stream live with [VocalStack](https://vocalstack.com/obs-studio-transcription-plugin/), and
see the captions over OBS's preview. Captions never go into your stream or recordings.

Read more about the [VocalStack live transcription plugin for OBS Studio](https://vocalstack.com/obs-studio-transcription-plugin/) on the [VocalStack website](https://vocalstack.com/).

- A **VocalStack dock** in OBS: sign in with one click (or an API key), name the transcription, pick a
  broadcast, set the spoken language and choose captions as spoken or translated. Speaker labels, the
  transcription mode (automatic, fast, accurate), an AI title and summary of each recording, and a lock
  against deletion are set from the dock too.
- **Start transcribing** from the dock or a hotkey, or let it start and stop with OBS's own streaming or
  recording. Each recording asks for the longest length your plan allows.
- The plugin sends OBS's audio (or audio and video) to VocalStack over its own connection, so the stream
  your viewers see is untouched. Transcriptions are saved in your VocalStack account; the dock opens them
  in VocalStack, copies a share link, and copies the broadcast's link (with how many are listening).
- **Updates install themselves**: when a newer release is out and nothing is being transcribed, the
  plugin installs it (macOS: the same signed package, into your Library; Linux: the .deb, after the
  desktop's password prompt; Windows: the zip, copied in when OBS quits) and asks for an OBS restart.
  Turn that off in the dock and an **Update now** button appears instead. A version a minor behind is
  marked old; one a major behind no longer starts.
- **Captions** over the preview name the speaker once more than one has been heard, follow corrections
  made in the dashboard, and fall back to the spoken language when VocalStack refuses a translation (a
  recording has at most ten languages).

## Download

Get the installer for your system from the [latest release](https://github.com/vocalstack/obs-plugin/releases/latest):

| System | File |
|---|---|
| Windows 10/11 (x64) | [vocalstack-obs-windows-x64.zip](https://github.com/vocalstack/obs-plugin/releases/latest/download/vocalstack-obs-windows-x64.zip) |
| macOS 12+ (Apple silicon and Intel) | [vocalstack-obs-macos.pkg](https://github.com/vocalstack/obs-plugin/releases/latest/download/vocalstack-obs-macos.pkg) |
| Ubuntu 24.04+ (x86_64) | [vocalstack-obs-linux-x86_64.deb](https://github.com/vocalstack/obs-plugin/releases/latest/download/vocalstack-obs-linux-x86_64.deb) |

Needs OBS Studio 31 or later.

- **Windows:** close OBS, press Windows + R, type `%ProgramData%\obs-studio\plugins` and press Enter (if it doesn't exist, create the folders `obs-studio\plugins` in `%ProgramData%`). Open the zip and drag its `vocalstack-obs` folder into `plugins`; Windows may ask for administrator permission. Start OBS.
- **macOS:** open the `.pkg` and follow the installer. Restart OBS.
- **Linux:** `sudo apt install ./vocalstack-obs-linux-x86_64.deb`, then restart OBS.

Then open **Docks → VocalStack** if the dock is not showing, and click **Sign in with VocalStack**.

## Build

Built from [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate); GitHub Actions builds every
push for Windows, macOS and Linux.

```sh
# macOS (Homebrew's xattr lacks -r, which the template's configure needs)
PATH=/usr/bin:$PATH cmake --preset macos
PATH=/usr/bin:$PATH cmake --build build_macos --config RelWithDebInfo
cp -R build_macos/RelWithDebInfo/vocalstack-obs.plugin \
  ~/Library/Application\ Support/obs-studio/plugins/
```

`VOCALSTACK_API` and `VOCALSTACK_APP` point the plugin at another VocalStack environment.

## Releasing

As VocalStack for Mac: bump `VERSION`, commit, tag the same version (`1.2.3`, no `v`) and push. CI
checks that the tag equals `VERSION`, builds Windows, macOS and Linux, publishes the GitHub release and
moves the `latest` release, whose download links never change
(`https://github.com/vocalstack/obs-plugin/releases/download/latest/vocalstack-obs-macos.pkg`, and so
on). The website's download links are updated by its own deploy.

The macOS build of a release is signed with the Developer ID (ETANOVA ENTERPRISE SOLUTIONS LIMITED, team
P94964SJFA; hardened runtime, secure timestamp) and the installer is signed, notarized by Apple and
stapled, with the same certificates and App Store Connect notary key as the Mac app, from the repository
secrets `DEVELOPER_ID_P12`, `DEVELOPER_ID_P12_PASSWORD`, `NOTARY_KEY_P8`, `NOTARY_KEY_ID` and
`NOTARY_ISSUER_ID`. The installer package also needs a **Developer ID Installer** certificate: in the
same `.p12`, or in `DEVELOPER_ID_INSTALLER_P12` (password `DEVELOPER_ID_INSTALLER_P12_PASSWORD`). A
release without them fails rather than ship unsigned; other builds are signed ad hoc.

## Privacy

The plugin sends nothing until you sign in. When you start transcribing it sends OBS's audio (or
audio and video, if you choose) to VocalStack, and nowhere else, to transcribe it into your VocalStack account;
see the [VocalStack Privacy Policy](https://vocalstack.com/privacy/). While signed in it also asks VocalStack
who the key is, the plan's limits, your broadcasts and languages, and follows the transcription's own event
feed while live. Signing out stops it and removes the plugin's stream key. Once you are signed in, it also asks GitHub every six hours whether a newer
release of the plugin is out, and downloads it from there to install it; those requests carry only the
plugin's version.

## Licence

GPLv2 or later, as OBS Studio. See [LICENSE](LICENSE).
