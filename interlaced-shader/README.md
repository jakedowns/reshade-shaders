## Looking Glass Portrait interlaced shader for Citra 3DS Emulator

### Basic Setup

1. In Citra; File > Open Citra Folder...

2. Go to Shaders directory and right-click to create a new text file called `lookingglass.glsl`

3. copy the contents of [lookingglass.glsl](./lookingglass.glsl) from this repo

4. Next, you need your calibration info.

> Each LKGP is uniquely calibrated, and those values need to be specified in the shader to get the correct output. In future versions, I might automate this step.
> Go to this URL and copy your Looking Glass Portrait's Calibration Data: https://jakedowns.github.io/looking-glass-calibration.html

5. in `lookingglass.glsl` replace my calibration values with yours

> **Note** you'll need at least Citra Nightly 1788 (2022-09-25) for this interlaced shader to load correctly.
> If you're using a mainline or nightly older than 2022-09-25, you'll need to perform the following extra step to allow the shader to load:
>
> copy your shader to /anaglyph/lookingglass.glsl subfolder within the shaders folder
> there's a bug where the shader file needs to be in two places to load properly citra-emu/citra#6133

6. Go to Emulation > Configure > Graphics

7. Set Stereoscopy > 3D Mode to Interlaced

8. Select Post-Processing Filter: lookingglass

9. enjoy! Let me know how it goes! https://twitter.com/jakedowns/status/1573681327005573120

### Advanced

if you want to try dialing in the sweet spot yourself, 
you can try altering the shader, you can press F9 or F10 or F11 in citra to reload the shader without restarting.

if it crashes, you've introduced a syntax error into the shader. undo and try again.

The main values to be tweaked are:
`float repeat = 100/3;` 
> // the number of times the LL/RR stereo pair will be repeated across the full viewing angle. 
> More repeats (100/4, 100/5) = more cross-talk, but more flexibility in viewing angles. 
> Lower divisions (100/1, 100/2) = less cross-talk, but there's only 1 or two "sweet spots"

`< repeat*0.5`
you can change this `0.5` to kind of... shift or rotate the sweet spot left or right

### Misc.

If the looking-glass-calibration url above doesn't work, here's a fallback version on codesandbox: 

https://codesandbox.io/s/floral-flower-w2sjmx?file=/index.html

If that doesn't work, try restarting Holoplay Service

### Related

If you want to try something a little more advanced, check out my [Citra addon for ReShade, ReGlass & Refract](https://github.com/jakedowns/reshade-shaders/tree/main/Citra%20AddOn)
