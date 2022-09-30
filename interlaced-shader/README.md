How to use Citra with Looking Glass Portrait

In Citra

File > Open Citra Folder...
Go to Shaders directory and right-click to create a new text file called lookingglass.glsl
copy lookingglass.glsl from this directory
Go to this URL and copy your Looking Glass Portrait's Calibration Data: https://jakedowns.github.io/looking-glass-calibration.html
Each LKGP is uniquely calibrated, and those values need to be specified in the shader to get the correct output. In future versions, I might automate this step.

replace my calibration values with yours
> TEMPORARY STEP UNTIL MY PR IS MERGED copy your shader to /anaglyph/lookingglass.glsl subfolder within the shaders folder
> there's a bug where the shader file needs to be in two places to load properly citra-emu/citra#6133

Go to Emulation > Configure > Graphics
Set Stereoscopy > 3D Mode to Interlaced
Select Post-Processing Filter: lookingglass
enjoy

Let me know how it goes! https://twitter.com/jakedowns/status/1573681327005573120

Advanced
if you want to try dialing in the sweet spot yourself, you can try altering the shader, you can press F9 or F10 or F11 in citra to reload the shader without restarting if it crashes, you've introduced a syntax error into the shader. undo and try again.

Misc.
If the looking-glass-calibration.html url above doesn't work, here's a fallback version on codesandbox: 
https://codesandbox.io/s/floral-flower-w2sjmx?file=/index.html 
If that doesn't work, try restarting Holoplay Service
