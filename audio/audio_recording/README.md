# audio_recording
This sample demonstrates how to integrate libretro's audio recording support into your core.

This core reads up to 5 seconds of input from the active microphone_interface, then plays it back.

If no microphone_interface is detected or the selected driver doesn't support it, an error message will be displayed on-screen.

Hold any button to enable the microphone_interface

## Programming language
C

## Building
To compile, you will need a C compiler and assorted toolchain installed.

	make
