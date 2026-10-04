uniform sampler2D gFine;
uniform sampler2D gCoarse;
uniform vec2 gCoarseTexelSize;
uniform float gScatter;

noperspective in vec2 TexCoord;
out vec4 FragClr;

void main()
{
	vec2 X = vec2(gCoarseTexelSize.x, 0.0);
	vec2 Y = vec2(0.0, gCoarseTexelSize.y);
	vec3 Coarse = texture(gCoarse, TexCoord).rgb * 4.0;
	Coarse += texture(gCoarse, TexCoord + X).rgb * 2.0;
	Coarse += texture(gCoarse, TexCoord - X).rgb * 2.0;
	Coarse += texture(gCoarse, TexCoord + Y).rgb * 2.0;
	Coarse += texture(gCoarse, TexCoord - Y).rgb * 2.0;
	Coarse += texture(gCoarse, TexCoord + X + Y).rgb;
	Coarse += texture(gCoarse, TexCoord + X - Y).rgb;
	Coarse += texture(gCoarse, TexCoord - X + Y).rgb;
	Coarse += texture(gCoarse, TexCoord - X - Y).rgb;
	Coarse *= 1.0 / 16.0;

	vec3 Fine = max(texture(gFine, TexCoord).rgb, vec3(0.0));
	FragClr = vec4(Fine + max(Coarse, vec3(0.0)) * gScatter, 1.0);
}
