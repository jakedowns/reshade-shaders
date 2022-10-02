## Citra Add-On + Citra.fx Effect for ReShade

### 🚧 \*\* **Work in Progress** \*\*

**Easier Alternative for Beginners** if ReShade is a little too advanced for you, try starting with the shader-only setup. No ReShade needed, just Citra + a custom [Looking Glass Interlacing Shader for Citra](https://github.com/jakedowns/reshade-shaders/tree/main/interlaced-shader)

### Features:
- normalizes Citra's depth buffers to be usable by other existing ReShade effects
  - rotates depth buffer so x,y uv coordinates are correct
  - inverts depth values so bright values are near and dark values are far
  - supports setting a fixed depth for the bottom screen when in split-screen mode

### How:
- basically a clone of the Generic Depth addon, that remaps all calls from other effects/addons looking to access the `DEPTH` texture to instead access a pre-processed depth texture, specially modified to normalize it from Citra's emulation-specific values to something more standardly consumable by other shader effects pipelines

### Installation:

> **Note** This addon requires you to install the version of reShade with full add on support
![image](https://user-images.githubusercontent.com/1683122/193439578-164b9797-dd68-4353-a3ae-5b9b48d564ee.png)

> **Note** in order to test this, you'll need a custom build of Citra. You can build it yourself from this [tagged commit](https://github.com/jakedowns/citra-fix-custom-interlaced-shader-path/tree/reshade-left-eye-optional), or [download the citra-qt.exe from the release page here](https://github.com/jakedowns/citra-fix-custom-interlaced-shader-path/releases/tag/reshade-left-eye-optional)
> 
> Hopefully the pull request to fix this gets merged: https://github.com/citra-emu/citra/pull/6140
> or, i'll find another way to capture the left-eye's depth buffer to negate the need for this patch


1. download [`citra.addon`](./citra.addon) (64-bit) OR [`citra32.addon`](./citra32.addon) (32-bit) into the same directory as `ReShade.ini` and `citra-qt.exe`
2. place `Citra.fx` in `./reshade-shaders/Shaders/` sub-directory within the Citra executable folder
4. start citra. (if it crashes or reshade doesn't launch, disable generic depth, edit ReShade.ini:
```
[ADDON]
DisabledAddons=Generic Depth
```
5. start citra. press `home` to bring up ReShade **Important** Disable the `Generic Depth` add-on
6. make sure `Citra` is enabled in the add-ons tab
7. make sure the `Citra` effect is enabled in the home tab, and that it's at the top of the effects list

<img width="400" src="https://user-images.githubusercontent.com/1683122/193273026-6a91450c-cc2c-4620-90cf-5ca975ce9a9c.png" /> <img width="400" src="https://user-images.githubusercontent.com/1683122/193273249-67039451-b3e5-4627-92e8-b7555ae69bf9.png" />


### Recommended / Tested Effects:

- *Looking Glass Portrait Support:*
  - [ReGlass](https://github.com/jbienz/ReGlass) aka [LookingGlass.fx](https://github.com/jbienz/ReGlass/blob/main/Shaders/LookingGlass.fx) 
  - with [Refract](https://github.com/SolerSoft/Refract)

- [MXAO.fx](https://github.com/cyrie/Stormshade/blob/master/reshade-shaders/Shaders/MXAO.fx)
- [CinematicDOF.fx](https://github.com/FransBouma/OtisFX/blob/master/Shaders/CinematicDOF.fx)
- mcflypg / Pascal Gilcher's ReShade Ray Tracing shader (RTGI) - https://www.patreon.com/mcflypg

### Known Issues

- currently, if you have too many, or too intense fx enabled, or resolution too high, you might see flickering.

  this is due to the fact that many games share a single depth buffer. i'm still working on a solution to prevent this.

- currently the "left, right, small" side-by-side modes are unfinished and require manual offset adjustments

- scaling adjustments and aspect adjustments need re-enabled

- certain window dimensions require a lot of manual offset adjustments, i'm working on automating these

### Credits
- [crosire](https://github.com/crosire/reshade) - Big thanks for giving us the guidance to get this add-on working
- [Edgarska](https://www.reddit.com/r/Citra/comments/i4o5i1/reshade_depth_buffer_access_fix/) - their reddit post in /r/ReShade, [Reshade depth buffer access fix.](https://www.reddit.com/r/Citra/comments/i4o5i1/reshade_depth_buffer_access_fix/) gave us a lot of the basics we needed to get this working. I simply packaged them up.
- Holophone3D - thanks for the motivation and inspiration to get this working. also thanks for getting the ball rolling with the basics for the shader / fx code.
- [jbienz](https://github.com/jbienz) / [SolerSoft](https://github.com/SolerSoft) - thanks for your enthusiasm and encouragement, and for your great work on ReGlass & Refract
- emufan4568 - thanks for your help on the Citra discord #development channel
- jakedowns - follow me on twitter.com/jakedowns for future updates | support my dev work on patreon.com/jakedownsthings | follow my art work on instagram.com/jakedownsthings

### Images
![image](https://user-images.githubusercontent.com/1683122/193262894-73fb5d86-0a54-4ef2-bf0d-61bba3a800a5.png)
