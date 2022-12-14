# audio_recording
This sample demonstrates how to integrate libretro's audio recording support into your core.

This core reads input from the active microphone and draws the waveform on-screen.

If no microphone is detected or the selected driver doesn't support it, an error message will be displayed on-screen.

## Programming language
C

## Building
To compile, you will need a C compiler and assorted toolchain installed.

	make
