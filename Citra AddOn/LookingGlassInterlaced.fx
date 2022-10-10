/*
   Learn More here: https://github.com/jakedowns/reshade-shaders/
*/

#include "ReShade.fxh"

// Calibration values 
// via: https://jakedowns.github.io/looking-glass-calibration.html
// this is especially made for looking glass portrait.
// TODO: make it more generic / able to support other variations
uniform float width <
    ui_type = "slider";
ui_min = 0.0; ui_max = 3000.0;
ui_step = 1.0;
> = 1536.0; //BUFFER_HEIGHT; 

uniform float height <
    ui_type = "slider";
ui_min = 0.0; ui_max = 3000.0;
ui_step = 1.0;
> = 2560.0; //BUFFER_WIDTH*1.6667; //BUFFER_WIDTH;

uniform float dpi <
    ui_type = "slider";
ui_min = 0.0; ui_max = 1000.0;
ui_step = 1.0;
> = 324.0f;

uniform float slope <
    ui_type = "slider";
//ui_category = "Preview Depth Buffer";
//ui_label = "slope";
ui_min = -10.0; ui_max = 10.0;
ui_step = 0.01;
> = -7.083540916442871;

uniform float center <
    ui_type = "slider";
ui_min = 0.0; ui_max = 1.0;
ui_step = 0.01;
> = 0.8167931437492371;

uniform float pitch <
    ui_type = "slider";
ui_min = 0.0; ui_max = 100.0;
ui_step = 0.1;
> = 52.59267044067383;

uniform float repeat <
    ui_type = "slider";
ui_min = 0.0; ui_max = 10.0;
ui_step = 0.1;
> = 2.0;

uniform float offset <
    ui_type = "slider";
ui_min = 0.0; ui_max = 2.0;
ui_step = 0.01;
> = 0.5;

float myatan(float y, float x)
{
    float pi = 3.14159265358979323846;
    if (x >= 0)
        return atan2(y, x);
    else if (y >= 0)
        return atan2(y, x) - pi;
    else
        return atan2(y, x) + pi;
}

float4 my_sample(float2 tex : TEXCOORD, float alpha) {
    float frepeat = 100 / repeat;
    float2 right_eye = float2(tex.xy);
    //right_eye.x *= 2.0;
    right_eye.x += 1.0;
    right_eye.x /= 2.0;

    float2 left_eye = float2(tex.xy);
    left_eye.x /= 2.0;

    // sample right eye
    // one-shot mode
    //if(frac(alpha) > 0.5){
    //if(frac(alpha) > 0.145){
    // if(alpha < -100){
    // repeated mode
    if ((frac(alpha) * 100) % frepeat >= frepeat * offset) {
        //return float4(1,0,0,1); // debug color
        return tex2D(ReShade::BackBuffer, right_eye);
    }
    // sample left eye
    //return float4(0,0,1,1); // debug color
    return tex2D(ReShade::BackBuffer, left_eye);
}

float4 LGSBS_PS(float4 pos : SV_POSITION, float2 tex : TEXCOORD) : SV_TARGET{
  float tilt = height / (width * slope);
  float pitch_adjusted = pitch * width / dpi * cos(myatan(1.0, slope));
  float subp = 1.0 / (3.0f * width) * pitch_adjusted;


  float alpha = (tex.x + tex.y * tilt) * pitch_adjusted - center;

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