/*
   Learn More here: https://github.com/jakedowns/reshade-shaders/
*/

#include "ReShade.fxh"

// Calibration values 
// via: https://jakedowns.github.io/looking-glass-calibration.html
uniform float slope = -7.083540916442871;
uniform float center = 0.8167931437492371;
uniform float pitch = 52.59267044067383;

// this is especially made for looking glass portrait.
// TODO: make it more generic / able to support other variations
uniform float width = 1536.0; //BUFFER_HEIGHT; 
uniform float height = 2560.0; //BUFFER_WIDTH*1.6667; //BUFFER_WIDTH;
uniform float dpi = 324.0f;

float4 my_sample(float2 tex : TEXCOORD, float alpha) {
    float repeat = 100 / 2;
    float2 right_eye = float2(tex.xy);
    //right_eye.x *= 2.0;
    right_eye.x += 1.0;
    right_eye.x /= 2.0;

    float2 left_eye = float2(tex.xy);
    left_eye.x /= 2.0;

    // sample right eye
    // one-shot mode
    if (frac(alpha) > 0.5) {
        //if(frac(alpha) > 0.145){
        // if(alpha < -100){
        // repeated mode
        //if(mod(fract(alpha)*100,repeat) >= repeat*0.5){
            //return float4(1,0,0,1); // debug color
        return tex2D(ReShade::BackBuffer, right_eye);
    }
    // sample left eye
    //return float4(0,0,1,1); // debug color
    return tex2D(ReShade::BackBuffer, left_eye);
}

float4 LGSBS_PS(float4 pos : SV_POSITION, float2 tex : TEXCOORD) : SV_TARGET{
  float tilt = height / (width * slope);
  float pitch_adjusted = pitch * width / dpi * cos(atan2(1.0, slope));
  float subp = 1.0 / (3.0f * width) * pitch_adjusted;


  float alpha = (tex.y + tex.x * tilt) * pitch_adjusted - center;

  float4 color = float4(0.0f,0.0f,0.0f,1.0f);
  color.r = my_sample(tex, alpha).r;
  color.g = my_sample(tex, alpha + subp).g;
  color.b = my_sample(tex, alpha + 2.0f * subp).b;

  return color;
}

technique LGSBS {
    pass {
        VertexShader = PostProcessVS;
        PixelShader = LGSBS_PS;
    }
}