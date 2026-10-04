noperspective out vec2 TexCoord;

void main()
{
	vec2 Position = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
	TexCoord = Position;
	gl_Position = vec4(Position * 2.0 - 1.0, 0.0, 1.0);
}
