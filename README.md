# reshade-shaders
Shaders for ReShade for Citra and other misc things

Gist Overview of adding Citra Support for ReShade

NOTE: i'm currently working on packaging all this up into a Citra add-on for ReShade. I'll post on reshade / citra subreddits when it's ready. feel free to follow me on twitter for updates as well: https://twitter.com/jakedowns You can also subscribe to this github repo for updates

https://gist.github.com/jakedowns/e6637f880e2fc3f9dfae5f34a6a8715c

### Devlog

#### 9.29.2022

made a little progress on my ReShade integration tonight.
I got some advice from the author crosire, who gave me a basic setup for how i can write out a modified depth buffer on the ReShade side, and then write an addon that will swap my modified / pre-formatted depth buffer for any additonal reshade effects that access it.

it's not totally working yet, but it is showing some promising signs of life.

(⚠ warning strobing / flashing lights https://www.dropbox.com/s/exefs6hchzumscz/citra-qt_d10BaVByKu.mp4?dl=0 )

excited to get this working.
here's the code for anyone interested: https://github.com/jakedowns/reshade-shaders/commit/68767eddaffe92ab32cf63cc2eefeec202d301b7

there is something interesting of note tho.

When StereoRenderOption is set to Off renderer_opengl renders the Left eye to the rgb buffer. However, by the time a frame completes, the one and only Depth buffer presented to ReShade is showing the depth/normals for the Right eye

on my local build, i hard-coded it to draw the right eye, but maybe i could add an option for Left or Right when 3d mode is set to off? happy to make the default Left if it needs to be.
