//!HOOK OUTPUT
//!BIND HOOKED
//!DESC LookingGlass

// Calibration values 
// via: https://jakedowns.github.io/looking-glass-calibration.html
float slope = -7.083540916442871;
float center = 0.8167931437492371;
float pitch = 52.59267044067383;

// this is especially made for looking glass portrait.
// TODO: make it more generic / able to support other variations
float width = 1536.0f;
float height = 2048.0f;
float dpi = 324.0f;

float tilt = height / (width * slope);
float pitch_adjusted = pitch * width / dpi * cos(atan(1.0f, slope));
float subp = 1.0f / (3.0f * width) * pitch_adjusted;
float repeat = 100.0f/3.0f;

vec4 my_sample(float alpha){
	vec2 pos = HOOKED_pos;
	float halfX = pos.x / 2.0f;
	if(mod(fract(alpha)*100.0f,repeat) < repeat*0.5){
		// left eye (left half of SBS);
		return HOOKED_tex(vec2(halfX, pos.y));
	}
	// right eye (right half of SBS)
	return HOOKED_tex(vec2(0.5 + halfX, pos.y));
}

vec4 hook(){

	vec4 myColor = HOOKED_tex(HOOKED_pos);

	float alpha = ( gl_FragCoord.x + gl_FragCoord.y * tilt ) * pitch_adjusted - center;

    // we sample 3 times since the r,g,b subpixels for each "original" pixel needs to be additionally shifted by one extra "subpixel" amount per channel to match the unique sub-pixel layout of the LKGP display
    myColor.r = my_sample(alpha).r;
    myColor.g = my_sample(alpha + subp).g;
    myColor.b = my_sample(alpha + 2.0f * subp).b;

    return myColor;
}
