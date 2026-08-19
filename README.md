# RTPlayer

RTPlayer is a derivative of [CoolPlayer](https://en.wikipedia.org/wiki/CoolPlayer), reworked as a lightweight music player for Windows on ARM32.

## Platform support

* Windows, ARM32 only (`armv7-w64-mingw32`)
* Tested and confirmed working on a jailbroken Surface RT (Windows RT 8.1)
* Not tested on any other Windows RT device — reports and pull requests covering other hardware are welcome

## Features

Inherited from CoolPlayer's engine, plus a touch-friendly UI rework on top:

* Native FLAC and OGG Vorbis tag reading, with ID3v1/ID3v2 support for MP3
* MAD mpeg engine
* Winamp input plugin support
* Advanced playlist editor
* Continuous play
* GNU General Public License

## FLAC playback

FLAC playback goes through a Winamp-style input plugin (`in_flac.dll`), built separately from [KiritoStudio/flac](https://github.com/KiritoStudio/flac). RTPlayer itself doesn't bundle libFLAC — to play FLAC files, build `in_flac.dll` from that repo and place it in the same directory as the RTPlayer executable (it's picked up automatically from there at startup).

## Contributing

Issues and pull requests are welcome, especially reports (or fixes) for other Windows RT/ARM32 devices this hasn't been tested on.
