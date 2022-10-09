## Citra Looking Glass Portrait Support / Citra ReShade Support

![image](https://user-images.githubusercontent.com/1683122/194764424-8130ca34-6f4b-40c7-a415-024b94a9affc.png)

### Method 1. Citra Addon + Citra Effect for ReShade:
https://github.com/jakedowns/reshade-shaders/tree/main/Citra%20AddOn

- more... fluid looking, since you're actually rendering more views
- doesn't cover all depth effects, some UI/Sprites/Transitions etc render oddly
- minor glitches (still a work in progress)

### Method 2. Looking Glass Portrait Interlaced Shader for Citra (no ReShade required)
https://github.com/jakedowns/reshade-shaders/tree/main/interlaced-shader

- better visual fidelity due to it simply displaying Left/Right images exactly as-rendered by 3DS emulator
- more eye strain since it's only 2 views and stereo views on a display that supports 100 is kind of silly (you kind of have to hold your head in a "Sweet-spot" but, it does look good when you're in the sweet-spot)
- option to have 1 sweet-spot (low cross talk, less forgiving for your neck) or repeating sweet-spots (more cross talk, more viable viewing angles)
