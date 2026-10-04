#include <game/client/components/items.h>

#include <engine/graphics.h>
#include <engine/shared/config.h>

void CItems::RenderZzLight(vec2 Pos, const ColorRGBA &Color, float Radius, float Alpha) const
{
	if(!g_Config.m_ClZzVisualEffects || !g_Config.m_ClZzVisualEffectsGameplay || !g_Config.m_ClZzVisualWeaponEffects || !g_Config.m_ClZzVisualDynamicLights || Alpha <= 0.001f)
		return;
	const float Strength = std::clamp(g_Config.m_ClZzVisualEffectsIntensity / 100.0f, 0.0f, 1.0f);
	Graphics()->BlendAdditive();
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	Graphics()->SetColor(Color.WithAlpha(Alpha * Strength * 0.055f));
	Graphics()->DrawCircle(Pos.x, Pos.y, Radius, 28);
	Graphics()->SetColor(Color.WithAlpha(Alpha * Strength * 0.14f));
	Graphics()->DrawCircle(Pos.x, Pos.y, Radius * 0.42f, 22);
	Graphics()->SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, Alpha * Strength * 0.08f));
	Graphics()->DrawCircle(Pos.x, Pos.y, Radius * 0.16f, 16);
	Graphics()->QuadsEnd();
	Graphics()->BlendNormal();
}

void CItems::RenderZzLaserLight(vec2 From, vec2 To, const ColorRGBA &Color, float Alpha) const
{
	if(!g_Config.m_ClZzVisualEffects || !g_Config.m_ClZzVisualEffectsGameplay || !g_Config.m_ClZzVisualWeaponEffects || !g_Config.m_ClZzVisualDynamicLights || Alpha <= 0.001f)
		return;
	const vec2 Delta = To - From;
	const float Len = length(Delta);
	if(Len <= 0.001f)
		return;
	const vec2 Dir = Delta / Len;
	const vec2 Normal(-Dir.y, Dir.x);
	const float Strength = std::clamp(g_Config.m_ClZzVisualEffectsIntensity / 100.0f, 0.0f, 1.0f);
	Graphics()->BlendAdditive();
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	Graphics()->SetColor(Color.WithAlpha(Alpha * Strength * 0.045f));
	const IGraphics::CFreeformItem BeamGlow(To + Normal * 18.0f, To - Normal * 18.0f, From + Normal * 18.0f, From - Normal * 18.0f);
	Graphics()->QuadsDrawFreeform(&BeamGlow, 1);
	Graphics()->SetColor(Color.WithAlpha(Alpha * Strength * 0.12f));
	Graphics()->DrawCircle(To.x, To.y, 27.0f, 28);
	Graphics()->DrawCircle(From.x, From.y, 18.0f, 24);
	Graphics()->QuadsEnd();
	Graphics()->BlendNormal();
}
