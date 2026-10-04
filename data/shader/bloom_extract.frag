uniform sampler2D gScene;
uniform float gThreshold;
uniform float gKnee;

noperspective in vec2 TexCoord;
out vec4 FragClr;

float SrgbToLinear(float Value)
{
	return Value <= 0.04045 ? Value / 12.92 :
		pow((Value + 0.055) / 1.055, 2.4);
}

vec3 SrgbToLinear(vec3 Color)
{
	return vec3(SrgbToLinear(Color.r), SrgbToLinear(Color.g),
		SrgbToLinear(Color.b));
}

void main()
{
	vec3 Color = SrgbToLinear(clamp(texture(gScene, TexCoord).rgb, 0.0, 1.0));
	float Luminance = dot(Color, vec3(0.2126, 0.7152, 0.0722));
	float Peak = max(max(Color.r, Color.g), Color.b);
	float Brightness = mix(Luminance, Peak, 0.62);
	float Threshold = SrgbToLinear(clamp(gThreshold, 0.0, 1.0));
	float Knee = max(gKnee, 0.0001);

	float Soft = clamp(Brightness - Threshold + Knee, 0.0, 2.0 * Knee);
	Soft = Soft * Soft / (4.0 * Knee);
	float Contribution = max(Brightness - Threshold, Soft);
	float HighlightWeight =
		clamp(Contribution / max(Brightness, 0.0001), 0.0, 1.0);

	// Colored weapon lights remain strong, while neutral white map tiles only
	// contribute a restrained highlight.
	float Chroma = Peak - min(min(Color.r, Color.g), Color.b);
	float Saturation = Chroma / max(Peak, 0.0001);
	float EmissiveWeight =
		mix(0.24, 1.0, smoothstep(0.07, 0.42, Saturation));
	vec3 BloomColor = mix(vec3(Luminance), Color, 1.08);
	FragClr =
		vec4(max(BloomColor, vec3(0.0)) * HighlightWeight * EmissiveWeight, 1.0);
}
