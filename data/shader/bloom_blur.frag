uniform sampler2D gSource;
uniform vec2 gTexelSize;
uniform vec2 gDirection;

noperspective in vec2 TexCoord;
out vec4 FragClr;

void main()
{
	vec2 Offset = gTexelSize * gDirection;
	vec3 Color = texture(gSource, TexCoord).rgb * 0.196482;
	Color += texture(gSource, TexCoord + Offset * 1.411765).rgb * 0.296907;
	Color += texture(gSource, TexCoord - Offset * 1.411765).rgb * 0.296907;
	Color += texture(gSource, TexCoord + Offset * 3.294118).rgb * 0.094470;
	Color += texture(gSource, TexCoord - Offset * 3.294118).rgb * 0.094470;
	Color += texture(gSource, TexCoord + Offset * 5.176471).rgb * 0.010382;
	Color += texture(gSource, TexCoord - Offset * 5.176471).rgb * 0.010382;
	FragClr = vec4(Color, 1.0);
}
