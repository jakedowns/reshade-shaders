// Citra 3DS Looking Glass Portrait Interlacing Shader
// September 24, 2022
// Created by Holophone3D and Jake Downs
// inspired by Ian Reese & Yann Vernier (lonetech)
// original source https://github.com/jakedowns/reshade-shaders/tree/main/interlaced-shader/

// Calibration values 
// via: https://jakedowns.github.io/looking-glass-calibration.html
float slope = -7.083540916442871;
float center = 0.8167931437492371;
float pitch = 52.59267044067383;

// this is especially made for looking glass portrait.
// TODO: make it more generic / able to support other variations
float width = o_resolution.y; 
float height = o_resolution.x;
float dpi = 324.0f;

float tilt = height / (width * slope);
float pitch_adjusted = pitch * width / dpi * cos(atan(1.0, slope));
float subp = 1.0 / (3.0f * width) * pitch_adjusted;
float repeat = 100/2;

vec4 my_sample(float alpha){
    // sample right eye
    
    // one-shot mode
    if(fract(alpha) > 0.145){
    // if(alpha < -100){
    // repeated mode
    //if(mod(fract(alpha)*100,repeat) >= repeat*0.5){
        // return vec4(1,0,0,1); // debug color
        return texture(color_texture_r, frag_tex_coord);
    }
    // sample left eye
    // return vec4(0,0,1,1); // debug color
    return texture(color_texture, frag_tex_coord);
}

void main() {
    // if(screen == 1){
    //     // bottom screen
    //     // straight texture read, nothing special to do
    //     color = texture(color_texture, frag_tex_coord);
    // }else{
        // top screen (3d interlaced for lenticular display)

        // alpha is... the offset for the subpixels for the current uv coordinate in the final interlaced texture,
        // which we assume is being drawn full screen, so 1536px wide

        // x and y are intentionally swapped here cause, 3DS LCDs are rotated, and the emulator maintains that
        // generate using our normalized uv
        float alpha = ( frag_tex_coord.y + frag_tex_coord.x * tilt ) * pitch_adjusted - center;

        // we sample 3 times since the r,g,b subpixels for each "original" pixel needs to be additionally shifted by one extra "subpixel" amount per channel to match the unique sub-pixel layout of the LKGP display
        color.r = my_sample(alpha).r;
        color.g = my_sample(alpha + subp).g;
        color.b = my_sample(alpha + 2.0f * subp).b;
    // }
}
