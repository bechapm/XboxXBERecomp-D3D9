# Steam Audio

The headers of Steam Audio 4.8.1 (`phonon.h`, `phonon_version.h`), Apache-2.0, from the release's `steamaudio/include`
(https://github.com/ValveSoftware/steam-audio/releases/tag/v4.8.1). The library itself is not here: a Windows build
downloads the release (`XBE_STEAM_AUDIO` in `cmake/XbeRecomp.cmake`) and puts `phonon.dll` and its notices,
`steamaudio-THIRDPARTY.md`, beside the executable. `runtime/audio/acoustics.cpp` loads it when it is first needed.
