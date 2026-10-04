uniform sampler2D gScene;
uniform sampler2D gBloom;
uniform vec2 gSceneTexelSize;
uniform float gBloomIntensity;
uniform float gExposure;
uniform float gSaturation;
uniform float gContrast;
uniform float gWarmth;
uniform float gVignette;
uniform float gAmbientLight;
uniform float gSharpening;

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

float LinearToSrgb(float Value)
{
	return Value <= 0.0031308 ? Value * 12.92 :
		1.055 * pow(Value, 1.0 / 2.4) - 0.055;
}

vec3 LinearToSrgb(vec3 Color)
{
	Color = max(Color, vec3(0.0));
	return vec3(LinearToSrgb(Color.r), LinearToSrgb(Color.g),
		LinearToSrgb(Color.b));
}

vec3 AcesFilm(vec3 Color)
{
	return clamp((Color * (2.51 * Color + 0.03)) /
		(Color * (2.43 * Color + 0.59) + 0.14), 0.0, 1.0);
}

vec3 SceneSample(vec2 Position)
{
	return SrgbToLinear(clamp(texture(gScene, Position).rgb, 0.0, 1.0));
}

void main()
{
	vec3 Scene = SceneSample(TexCoord);
	float BlurAmount = clamp(-gSharpening, 0.0, 1.0);
		float SampleRadius = mix(1.5, 7.5, BlurAmount);
	vec2 SampleStep = gSceneTexelSize * SampleRadius;
	vec3 Neighbors = SceneSample(TexCoord + vec2(SampleStep.x, 0.0));
	Neighbors += SceneSample(TexCoord - vec2(SampleStep.x, 0.0));
	Neighbors += SceneSample(TexCoord + vec2(0.0, SampleStep.y));
	Neighbors += SceneSample(TexCoord - vec2(0.0, SampleStep.y));
	Neighbors *= 0.25;
	if(BlurAmount > 0.0)
	{
		vec3 Diagonals = SceneSample(TexCoord + SampleStep);
		Diagonals += SceneSample(TexCoord - SampleStep);
		Diagonals += SceneSample(TexCoord + vec2(SampleStep.x, -SampleStep.y));
		Diagonals += SceneSample(TexCoord + vec2(-SampleStep.x, SampleStep.y));
		Diagonals *= 0.25;
		vec3 SoftScene = Scene * 0.20 + Neighbors * 0.48 + Diagonals * 0.32;
		Scene = mix(Scene, SoftScene, BlurAmount);
		Neighbors = mix(Neighbors, SoftScene, BlurAmount);
	}

	// Contrast-adaptive sharpening avoids bright outlines on hard tile edges.
	vec3 Edge = Scene - Neighbors;
	float EdgeRange = max(max(abs(Edge.r), abs(Edge.g)), abs(Edge.b));
	float Adaptive = 1.0 - smoothstep(0.08, 0.42, EdgeRange);
	vec3 Sharpened = max(Scene + Edge * max(gSharpening, 0.0) *
		(0.35 + Adaptive * 0.65), vec3(0.0));

	vec3 Bloom = max(texture(gBloom, TexCoord).rgb, vec3(0.0));
	float SceneLuminance = dot(Scene, vec3(0.2126, 0.7152, 0.0722));
	float EdgeOcclusion = smoothstep(0.07, 0.34, EdgeRange);
	float DarkEdge = 1.0 - smoothstep(0.035, 0.34, SceneLuminance);
	Bloom *= 1.0 - EdgeOcclusion * DarkEdge * 0.68;
	float BloomLuminance = dot(Bloom, vec3(0.2126, 0.7152, 0.0722));
	float LocalLight = smoothstep(0.008, 0.24, BloomLuminance);
	vec3 Color = Sharpened * mix(gAmbientLight, 1.0, LocalLight);

	// Soft screen-space lighting keeps the source readable while spreading its
	// color into the nearby world.
	vec3 BloomContribution = Bloom * gBloomIntensity;
	Color += BloomContribution *
		(vec3(1.0) - clamp(Sharpened, 0.0, 1.0) * 0.42);
	Color *= max(gExposure, 0.01);

	float HdrPeak = max(max(Color.r, Color.g), Color.b);
	vec3 Filmic = AcesFilm(Color);
	float FilmicAmount =
		0.30 + smoothstep(0.45, 1.65, HdrPeak) * 0.62;
	Color = mix(clamp(Color, 0.0, 1.0), Filmic, FilmicAmount);

	float Luminance = dot(Color, vec3(0.2126, 0.7152, 0.0722));
	float Chroma = max(max(Color.r, Color.g), Color.b) -
		min(min(Color.r, Color.g), Color.b);
	float Vibrance =
		max(gSaturation - 1.0, 0.0) * (1.0 - clamp(Chroma, 0.0, 1.0));
	Color = mix(vec3(Luminance), Color, gSaturation + Vibrance * 0.55);
	Color = (Color - 0.18) * gContrast + 0.18;

	float MidtoneMask =
		clamp(4.0 * Luminance * (1.0 - Luminance), 0.0, 1.0);
	Color += vec3(0.045, 0.012, -0.029) * gWarmth * MidtoneMask;

	vec2 Centered = TexCoord - vec2(0.5);
	float EdgeMask = smoothstep(0.12, 0.50, dot(Centered, Centered));
	Color *= 1.0 - EdgeMask * gVignette;
	Color = LinearToSrgb(clamp(Color, 0.0, 1.0));

	float Dither =
		fract(sin(dot(TexCoord, vec2(12.9898, 78.233))) * 43758.5453);
	Color += (Dither - 0.5) / 510.0;
	FragClr = vec4(clamp(Color, 0.0, 1.0), 1.0);
}
