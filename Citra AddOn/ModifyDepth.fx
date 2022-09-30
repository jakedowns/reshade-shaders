/*
  Citra by Holophone3D, Jake Downs, Jared Bienz.
  Recommended to be used with:
  https://github.com/jbienz/ReGlass & https://github.com/SolerSoft/Refract

  Preprocess depth map textures from Citra so they can be used by other add-ons
  - pre-swap (un-rotate) depth buffer xy coordinates

*/

#include "ReShade.fxh"

// -- Options --

uniform float BUFFER_AR =  BUFFER_HEIGHT / BUFFER_WIDTH;

uniform int iUIBottomScreenPosition <
  ui_type = "combo";
  ui_label = "Bottom Screen Position";
  ui_category = "Bottom Screen";
  ui_items = "Bottom\0"
             "Top\0"
             "Left\0"
             "Right\0"
             "Small\0"
             "Disabled\0";
> = 0;

uniform float fUIBottomFocus <
  ui_type = "drag";
  ui_label = "Bottom Screen Focus";
  ui_category = "Bottom Screen";
  ui_tooltip = "Adjust bottom screen near/far focus.\n";
  ui_min = 0.0; ui_max = 1.0;
  ui_step = 0.05;
> = 0.5;

uniform bool bUIPreviewDepth <
  ui_category = "Preview Depth Buffer";
  ui_label = "Preview Depth Buffer";
> = false;

uniform float bUIPreviewAlpha <
	ui_type = "drag";
	ui_category = "Preview Depth Buffer";
	ui_label = "Preview Alpha";
	ui_min = 0.0; ui_max = 1.0;
> = 1.0;

uniform float fUINearPlane <
  ui_type = "drag";
  ui_label = "Near Plane";
  ui_category = "Depth";
  ui_tooltip = "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=<value>";
  ui_min = 0.0; ui_max = 1000.0;
  ui_step = 0.01;
> = 0.0;

uniform float fUIFarPlane <
  ui_type = "drag";
  ui_label = "Far Plane";
  ui_category = "Depth";
  ui_tooltip = "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=<value>";
  ui_min = 0.0; ui_max = 1000.0;
  ui_step = 0.01;
> = 0.20;

uniform float fUIDepthMultiplier <
  ui_type = "drag";
  ui_label = "Multiplier";
  ui_category = "Depth";
  ui_tooltip = "RESHADE_DEPTH_MULTIPLIER=<value>";
  ui_min = 0.0; ui_max = 1000.0;
  ui_step = 0.01;
> = 1.0;

// -- Aspect Options --

uniform bool bUIUseCustomAspectRatio <
	ui_label = "Use Custom Aspect Ratio";
	ui_category = "Aspect ratio";
> = false;

uniform float AspectRatio <
	ui_type = "drag";
	ui_label = "Correct proportions";
	ui_category = "Aspect ratio";
	ui_min = -4.0; ui_max = 4.0;
	ui_step = 0.01;
> = 1.0;

uniform float ScaleX <
  ui_type = "drag";
	ui_label = "Scale image X";
	ui_category = "Aspect ratio";
	ui_min = 0.0; ui_max = 4.0;
	ui_step = 0.001;
> = 1.0;

uniform float ScaleY <
  ui_type = "drag";
	ui_label = "Scale image Y";
	ui_category = "Aspect ratio";
	ui_min = 0.0; ui_max = 4.0;
	ui_step = 0.001;
> = 1.0;

uniform float fUIDepthXOffset <
	ui_label = "Offset X Relative";
	ui_category = "Offsets";
	ui_type = "drag";
	ui_step = 0.001;
> = 0.0;

// uniform float fUIDepthXPxOffset <
// 	ui_label = "Offset X in pixels";
// 	ui_category = "Offsets";
// 	ui_type = "drag";
// > = 0.0;

uniform float fUIDepthYOffset <
	ui_label = "Offset Y Relative";
	ui_category = "Offsets";
	ui_type = "drag";
	ui_step = 0.001;
> = 0.0;

// uniform float fUIDepthYPxOffset <
// 	ui_label = "Offset Y in pixels";
// 	ui_category = "Offsets";
// 	ui_type = "drag";
// > = 0.0;

uniform bool FitScreen <
	ui_label = "Scale image to borders";
	ui_category = "Aspect ratio";
> = true;

uniform float4 Color <
	ui_label = "Background color";
	ui_category = "Aspect ratio";
	ui_type = "color";
> = float4(0.027, 0.027, 0.027, 0.17);

// uniform int iUIPresentType <
//   ui_type = "combo";
//   ui_label = "Present type";
//   ui_items = "Depth map\0"
//              "Normal map\0"
//              "Show both (Vertical 50/50)\0";
// > = 2;

texture OrigDepthTex : DEPTH;
sampler OrigDepth{ Texture = OrigDepthTex; };

texture ModifiedDepthTex{ Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R32F; };

float3 AspectRatioPS(
	float4 pos : SV_Position, 
	float2 texcoord : TEXCOORD,
	float aspect,
	float zoom,
	bool fitscreen,
	int depth 
) : SV_Target
{
	bool mask = false;

	// Center coordinates
	float2 coord = texcoord-0.5;

	// if (Zoom != 1.0) coord /= Zoom;
	if (zoom != 1.0) coord /= clamp(zoom, 1.0, 1.5); // Anti-cheat

	// Squeeze horizontally
	if (aspect<0)
	{
		coord.x *= abs(aspect)+1.0; // Apply distortion

		// Scale to borders
		if (fitscreen) coord /= abs(aspect)+1.0;
		else // mask image borders
			mask = abs(coord.x)>0.5;
	}
	// Squeeze vertically
	else if (aspect>0)
	{
		coord.y *= aspect+1.0; // Apply distortion

		// Scale to borders
		if (fitscreen) coord /= abs(aspect)+1.0;
		else // mask image borders
			mask = abs(coord.y)>0.5;
	}

	// Coordinates back to the corner
	coord += 0.5;

	// Sample display image and return
	if(mask){ 
		return Color.rgb;
	}

	return depth ? tex2D(OrigDepth, coord).rgb : tex2D(ReShade::BackBuffer, coord).rgb;
}

bool isBottomScreenPx(float2 input_tex, inout float2 texcoord){

	// disabled? return false
	if(iUIBottomScreenPosition == 5){
    return false;
  }

  bool isBottomScreenPixel = false;
  if(iUIBottomScreenPosition == 0){
    // bottom
    isBottomScreenPixel = input_tex.y > 0.5;
  }else if(iUIBottomScreenPosition == 1){
    // top
    isBottomScreenPixel = input_tex.y < 0.5;
  }else if(iUIBottomScreenPosition == 2){
    // left
    isBottomScreenPixel = input_tex.x < 1.0 - 0.55546875;
  }else if(iUIBottomScreenPosition == 3){
    // right
    isBottomScreenPixel = input_tex.x > 0.55546875;
  }else if(iUIBottomScreenPosition == 4){
  	// right > small
  	isBottomScreenPixel = input_tex.x > 0.83333333;
  }

  int2 depthSize = tex2Dsize(OrigDepth);
  depthSize = int2(depthSize.yx);

  if(!isBottomScreenPixel){
  	if(iUIBottomScreenPosition == 0){
	    // bottom
	    texcoord.x = texcoord.x * 2.0;
	    texcoord.x = texcoord.x - 0.5;

	    float horiz_px_width = (BUFFER_HEIGHT / 2) * depthSize.x / (depthSize.y/2);
	    float rel_width = horiz_px_width / BUFFER_WIDTH;
	    texcoord.y = texcoord.y / rel_width;
	    // need to figure out how to derrive this number
	    // 1.67 would be 400/240, but that's too far
	    // guess i could do 

	    float MAGIC_NUMBER = 1.57; //1.57; //(depthSize.x / (depthSize.y/2)) - .1; 
	    // but where does the .1 come from?
	    texcoord.y = texcoord.y - (rel_width * MAGIC_NUMBER); 
	  }else if(iUIBottomScreenPosition == 1){
	    // top
	    texcoord.x = texcoord.x * 2.0;

	    float horiz_px_width = BUFFER_HEIGHT * depthSize.x / depthSize.y;
	    float rel_width = horiz_px_width / BUFFER_WIDTH;
	    texcoord.y = texcoord.y / rel_width;
	    // need to figure out how to derrive this number
	    texcoord.y = texcoord.y - (rel_width * 1.57); 
	  }else if(iUIBottomScreenPosition == 2){
	    // left
	    // texcoord.x = texcoord.x * 2.0;
	    // texcoord.y = texcoord.y * 2.0;
	    // texcoord.x = texcoord.x - 0.55546875;
	  }else if(iUIBottomScreenPosition == 3){
	    // right
	  	// in memory, the texture is always top over bottom, 400x480
	  	// in citra's display tho, the rgb texture in this mode is
	  	// sbs 400+320 by 240
	    float2 virtualTextureSize = float2(
	    	depthSize.x*1.5556, // 400 + 320 = 720; 400/720 = .5556
	    	depthSize.y/2.0 // 240 * 2 = 480; 480 / 2 = 240
	    );
	    float vert_px_height = (BUFFER_WIDTH*1.0) * virtualTextureSize.y / virtualTextureSize.x;
	    float vert_rel_height = vert_px_height / BUFFER_HEIGHT;
	    texcoord.x = texcoord.x / vert_rel_height;
	    float offsety = (1.0 - vert_rel_height) / 2;
	    texcoord.x = texcoord.x - 0.07; //offsety;


	    texcoord.y = texcoord.y / 0.5556;
	    float scaledWidth = vert_px_height * 3;
	    float topScreenWidth = scaledWidth; // * .5556;
	    float offsetx = topScreenWidth;
	    offsetx = offsetx / BUFFER_WIDTH;
	    texcoord.y = texcoord.y - 0.8;
	    
	    
	  }else if(iUIBottomScreenPosition == 4){
	   	// right small
	   	texcoord.y = texcoord.y * 1.22;
	   	texcoord.y = texcoord.y - (1.0 - 0.83333333);
	  }else {
	  	// fullscreen

	  }

	  texcoord.x = texcoord.x / ScaleY;
		texcoord.y = texcoord.y / ScaleX;

	  if(bUIUseCustomAspectRatio){
	  	texcoord.x = texcoord.x * AspectRatio;
	  	texcoord.y = texcoord.y * AspectRatio;
	  }
	  // else
	  // 	texcoord.y = texcoord.y * BUFFER_AR;
  }
  // return false;

  return isBottomScreenPixel;
}

float GetModDepth(float2 tex : TEXCOORD) {
	float2 mytex = float2(tex);
	float2 input_tex = float2(tex);

	float citra_x = mytex.x;
	mytex.x = mytex.y;
	mytex.y = citra_x;
  // mytex.x = 1.0 - mytex.x;
  // mytex.y = 1.0 - mytex.y;
  mytex = 1.0 - mytex;
  mytex.x /= 2.0; // ignore bottom half

  if (isBottomScreenPx(input_tex, mytex)){
    return fUIBottomFocus;
  }

  // if(bUIDepthIsUpsideDown){
  // 	tex.y = 1.0 - tex.y;
  // }

  // mytex.x /= fUIDepthXScale;
  // mytex.y /= fUIDepthYScale;	

  if(fUIDepthXOffset){
  	mytex.y += fUIDepthXOffset / 2.000000001;
  }
  // else if(fUIDepthXPxOffset){
		// mytex.x -= fUIDepthXPxOffset * BUFFER_RCP_WIDTH;
  // }

  if(fUIDepthYOffset){
  	mytex.x -= fUIDepthYOffset / 2.000000001;
  }
  // else if(fUIDepthYPxOffset){
  // 	mytex.y += fUIDepthYPxOffset * BUFFER_RCP_HEIGHT;
  // }
 

	float depth = tex2Dlod(OrigDepth, float4(mytex, 0, 0)).x * fUIDepthMultiplier;

	// if(bUIDepthIsLog){
	// 	const float C = 0.01;
  //  depth = (exp(depth * log(C + 1.0)) - 1.0) / C;
	// }

	// invert by default for citra
	depth = 1.0 - depth;
	// if(!bUIDepthIsReversed){
  // 	depth = 1.0 - depth;
  // }

	const float N = 1.0;
  depth /= fUIFarPlane - depth * (fUIFarPlane - N);

	return depth;
}

float4 MyPS(float4 pos : SV_POSITION, float2 tex : TEXCOORD) : SV_TARGET {
	float depth = GetModDepth(tex);
	return float4(depth.xxx,0.0);
}

float4 PreviewDepth(float4 pos : SV_POSITION, float2 tex : TEXCOORD) : SV_TARGET {
	if(bUIPreviewDepth){
		float depth = GetModDepth(tex);
		return lerp(tex2D(ReShade::BackBuffer, tex), float4(depth.xxx,0.0), bUIPreviewAlpha);
	}
	return tex2D(ReShade::BackBuffer, tex);
}


// FullscreenVS
technique ModifyDepth {
	pass {
		VertexShader = PostProcessVS;
		PixelShader = MyPS;
		RenderTarget = ModifiedDepthTex;
	}
	pass {
		VertexShader = PostProcessVS;
		PixelShader = PreviewDepth;
	}
}
