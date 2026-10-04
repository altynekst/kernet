uniform sampler2D gSource;
uniform vec2 gTexelSize;

noperspective in vec2 TexCoord;
out vec4 FragClr;

vec3 WeightedSample(vec2 Offset, inout float WeightSum)
{
	vec3 Color = max(texture(gSource, TexCoord + Offset).rgb, vec3(0.0));
	float Luminance = dot(Color, vec3(0.2126, 0.7152, 0.0722));
	float Weight = 1.0 / (1.0 + Luminance);
	WeightSum += Weight;
	return Color * Weight;
}

void main()
{
	vec2 X = vec2(gTexelSize.x, 0.0);
	vec2 Y = vec2(0.0, gTexelSize.y);
	float WeightSum = 0.0;
	vec3 Color = WeightedSample(vec2(0.0), WeightSum);
	Color += WeightedSample(X, WeightSum);
	Color += WeightedSample(-X, WeightSum);
	Color += WeightedSample(Y, WeightSum);
	Color += WeightedSample(-Y, WeightSum);
	Color += WeightedSample(X + Y, WeightSum);
	Color += WeightedSample(X - Y, WeightSum);
	Color += WeightedSample(-X + Y, WeightSum);
	Color += WeightedSample(-X - Y, WeightSum);
	Color += WeightedSample(X * 2.0, WeightSum);
	Color += WeightedSample(-X * 2.0, WeightSum);
	Color += WeightedSample(Y * 2.0, WeightSum);
	Color += WeightedSample(-Y * 2.0, WeightSum);
	FragClr = vec4(Color / max(WeightSum, 0.0001), 1.0);
}
