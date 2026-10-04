#include <game/client/components/effects.h>

#include <base/math.h>

#include <engine/shared/config.h>

#include <generated/client_data.h>
#include <generated/protocol.h>

#include <game/client/components/particles.h>
#include <game/client/gameclient.h>

#include <algorithm>
#include <cmath>

void CEffects::ZzLanding(vec2 Pos, float Speed, const ColorRGBA &Color, float Alpha)
{
	if(!g_Config.m_ClZzVisualEffects || !g_Config.m_ClZzVisualEffectsGameplay || !g_Config.m_ClZzVisualPlayerMotion)
		return;
	const float Strength = std::clamp(g_Config.m_ClZzVisualEffectsIntensity / 100.0f, 0.0f, 1.0f) * m_ZzQuality;
	const int Count = std::clamp((int)std::round((4.0f + Speed * 0.55f) * Strength), 2, 12);
	for(int i = 0; i < Count; ++i)
	{
		const float Side = (i & 1) ? 1.0f : -1.0f;
		const float GroundOffsetX = Side * random_float(6.0f, 17.0f);
		CParticle p;
		p.SetDefault();
		p.m_Spr = (i & 1) ? SPRITE_PART_SHELL : SPRITE_PART_SMOKE;
		p.m_Pos = Pos + vec2(GroundOffsetX, 21.0f + random_float(-1.0f, 1.0f));
		p.m_Vel = vec2(Side * random_float(55.0f, 145.0f), random_float(-92.0f, -38.0f));
		p.m_LifeSpan = random_float(0.24f, 0.52f);
		p.m_StartSize = random_float(4.0f, 9.0f);
		p.m_EndSize = 0.0f;
		p.m_UseAlphaFading = true;
		p.m_StartAlpha = Alpha * Strength * 0.65f;
		p.m_EndAlpha = 0.0f;
		p.m_Gravity = random_float(210.0f, 390.0f);
		p.m_Friction = 0.72f;
		p.m_FlowAffected = 0.0f;
		p.m_Collides = false;
		p.m_Color = Color.WithAlpha(p.m_StartAlpha);
		GameClient()->m_Particles.Add(CParticles::GROUP_GENERAL, &p);
	}
}

void CEffects::ZzMotionTrail(vec2 Pos, vec2 Vel, const ColorRGBA &Color, float Alpha, bool OtherPlayer)
{
	if(!m_Add50hz || !g_Config.m_ClZzVisualEffects || !g_Config.m_ClZzVisualEffectsGameplay || !g_Config.m_ClZzVisualPlayerMotion ||
		(OtherPlayer && !g_Config.m_ClZzVisualOtherTrails))
		return;
	const float Speed = length(Vel);
	if(Speed < (OtherPlayer ? 7.2f : 6.0f) || random_float() > m_ZzQuality)
		return;
	const float Strength = std::clamp(g_Config.m_ClZzVisualEffectsIntensity / 100.0f, 0.0f, 1.0f);
	const vec2 Dir = Vel / Speed;
	CParticle p;
	p.SetDefault();
	p.m_Spr = SPRITE_PART_SPARKLE;
	p.m_Pos = Pos - Dir * random_float(10.0f, 22.0f) + random_direction() * random_float(1.0f, 5.0f);
	p.m_Vel = -Dir * random_float(18.0f, 55.0f);
	p.m_LifeSpan = OtherPlayer ? 0.26f : 0.34f;
	p.m_StartSize = random_float(5.0f, 9.0f);
	p.m_EndSize = 0.0f;
	p.m_UseAlphaFading = true;
	p.m_StartAlpha = Alpha * Strength * (OtherPlayer ? 0.25f : 0.46f);
	p.m_EndAlpha = 0.0f;
	p.m_Friction = 0.78f;
	p.m_FlowAffected = 0.0f;
	p.m_Collides = false;
	p.m_Color = Color.WithAlpha(p.m_StartAlpha);
	GameClient()->m_Particles.Add(CParticles::GROUP_TRAIL_EXTRA, &p);
}

void CEffects::ZzFreezeVapor(vec2 Pos, float Alpha)
{
	if(!m_Add5hz || !g_Config.m_ClZzVisualEffects || !g_Config.m_ClZzVisualEffectsFreeze || random_float() > m_ZzQuality)
		return;
	const float Strength = std::clamp(g_Config.m_ClZzVisualEffectsIntensity / 100.0f, 0.0f, 1.0f);
	CParticle p;
	p.SetDefault();
	const bool Snowflake = random_float() < 0.45f;
	p.m_Spr = Snowflake ? SPRITE_PART_SNOWFLAKE : SPRITE_PART_SMOKE;
	p.m_Pos = Pos + vec2(random_float(-18.0f, 18.0f), random_float(-12.0f, 14.0f));
	p.m_Vel = vec2(random_float(-14.0f, 14.0f), random_float(-46.0f, -20.0f));
	p.m_LifeSpan = random_float(0.75f, 1.35f);
	p.m_StartSize = random_float(5.0f, 12.0f);
	p.m_EndSize = p.m_StartSize * 0.35f;
	p.m_UseAlphaFading = true;
	p.m_StartAlpha = Alpha * Strength * 0.42f;
	p.m_EndAlpha = 0.0f;
	p.m_Friction = 0.9f;
	p.m_FlowAffected = 0.0f;
	p.m_Collides = false;
	p.m_Color = ColorRGBA(0.48f, 0.84f, 1.0f, p.m_StartAlpha);
	GameClient()->m_Particles.Add(Snowflake ? CParticles::GROUP_EXTRA : CParticles::GROUP_GENERAL, &p);
}

void CEffects::ZzFreezeBreak(vec2 Pos, float Alpha)
{
	if(!g_Config.m_ClZzVisualEffects || !g_Config.m_ClZzVisualEffectsFreeze)
		return;
	const float Strength = std::clamp(g_Config.m_ClZzVisualEffectsIntensity / 100.0f, 0.0f, 1.0f) * m_ZzQuality;
	const int Count = std::max(4, (int)std::round(13.0f * Strength));
	for(int i = 0; i < Count; ++i)
	{
		const bool Snowflake = i % 3 == 0;
		CParticle p;
		p.SetDefault();
		p.m_Spr = Snowflake ? SPRITE_PART_SNOWFLAKE : SPRITE_PART_SLICE;
		p.m_Pos = Pos + random_direction() * random_float(2.0f, 14.0f);
		p.m_Vel = random_direction() * random_float(75.0f, 185.0f) + vec2(0.0f, -35.0f);
		p.m_LifeSpan = random_float(0.38f, 0.8f);
		p.m_StartSize = random_float(5.0f, 12.0f);
		p.m_EndSize = 0.0f;
		p.m_UseAlphaFading = true;
		p.m_StartAlpha = Alpha * Strength * 0.82f;
		p.m_EndAlpha = 0.0f;
		p.m_Rot = random_angle();
		p.m_Rotspeed = random_float(-4.0f, 4.0f);
		p.m_Gravity = 280.0f;
		p.m_Friction = 0.86f;
		p.m_FlowAffected = 0.0f;
		p.m_Collides = false;
		p.m_Color = ColorRGBA(0.62f, 0.9f, 1.0f, p.m_StartAlpha);
		GameClient()->m_Particles.Add(Snowflake ? CParticles::GROUP_EXTRA : CParticles::GROUP_GENERAL, &p);
	}
}

void CEffects::ZzWeaponBurst(vec2 Pos, vec2 Dir, int Weapon, float Alpha)
{
	if(!g_Config.m_ClZzVisualEffects || !g_Config.m_ClZzVisualEffectsGameplay || !g_Config.m_ClZzVisualWeaponEffects)
		return;
	const ColorRGBA Color = ZzWeaponColor(Weapon);
	int BaseCount = 5;
	switch(Weapon)
	{
	case WEAPON_HAMMER: BaseCount = 8; break;
	case WEAPON_GUN: break;
	case WEAPON_LASER: BaseCount = 7; break;
	case WEAPON_GRENADE: BaseCount = 9; break;
	default: return;
	}
	const float Strength = std::clamp(g_Config.m_ClZzVisualEffectsIntensity / 100.0f, 0.0f, 1.0f) * m_ZzQuality;
	const int Count = std::max(2, (int)std::round(BaseCount * Strength));
	for(int i = 0; i < Count; ++i)
	{
		CParticle p;
		p.SetDefault();
		p.m_Spr = (Weapon == WEAPON_GRENADE && (i & 1)) ? SPRITE_PART_SMOKE : SPRITE_PART_SLICE;
		p.m_Pos = Pos + random_direction() * random_float(1.0f, 5.0f);
		p.m_Vel = Dir * random_float(45.0f, 140.0f) + random_direction() * random_float(25.0f, 105.0f);
		p.m_LifeSpan = random_float(0.18f, 0.46f);
		p.m_StartSize = random_float(4.0f, 10.0f);
		p.m_EndSize = 0.0f;
		p.m_UseAlphaFading = true;
		p.m_StartAlpha = Alpha * Strength * 0.8f;
		p.m_EndAlpha = 0.0f;
		p.m_Friction = 0.72f;
		p.m_Gravity = Weapon == WEAPON_GRENADE ? -70.0f : 0.0f;
		p.m_FlowAffected = 0.0f;
		p.m_Collides = false;
		p.m_Color = Color.WithAlpha(p.m_StartAlpha);
		GameClient()->m_Particles.Add(CParticles::GROUP_GENERAL, &p);
	}
}

ColorRGBA CEffects::ZzWeaponColor(int Weapon) const
{
	unsigned Color = g_Config.m_ClZzWeaponGunColor;
	switch(Weapon)
	{
	case WEAPON_HAMMER: Color = g_Config.m_ClZzWeaponHammerColor; break;
	case WEAPON_LASER: Color = g_Config.m_ClZzWeaponLaserColor; break;
	case WEAPON_GRENADE: Color = g_Config.m_ClZzWeaponGrenadeColor; break;
	default: break;
	}
	return color_cast<ColorRGBA>(ColorHSLA(Color));
}

void CEffects::ZzPlayerPresence(vec2 Pos, const ColorRGBA &Color, bool Appear, float Alpha)
{
	if(!g_Config.m_ClZzVisualEffects || !g_Config.m_ClZzVisualEffectsGameplay || !g_Config.m_ClZzVisualPlayerPresence)
		return;
	const float Strength = std::clamp(g_Config.m_ClZzVisualEffectsIntensity / 100.0f, 0.0f, 1.0f) * m_ZzQuality;
	const int Count = std::max(5, (int)std::round((Appear ? 13.0f : 10.0f) * Strength));
	for(int i = 0; i < Count; ++i)
	{
		CParticle p;
		p.SetDefault();
		p.m_Spr = SPRITE_PART_SLICE;
		const vec2 Dir = random_direction();
		p.m_Pos = Pos + Dir * random_float(5.0f, 22.0f);
		p.m_Vel = Dir * random_float(Appear ? -85.0f : 55.0f, Appear ? -25.0f : 145.0f) + vec2(0.0f, Appear ? 22.0f : -28.0f);
		p.m_LifeSpan = random_float(0.35f, 0.72f);
		p.m_StartSize = Appear ? random_float(2.0f, 6.0f) : random_float(5.0f, 10.0f);
		p.m_EndSize = Appear ? random_float(7.0f, 12.0f) : 0.0f;
		p.m_UseAlphaFading = true;
		p.m_StartAlpha = Alpha * Strength * 0.72f;
		p.m_EndAlpha = 0.0f;
		p.m_Rot = random_angle();
		p.m_Rotspeed = random_float(-3.0f, 3.0f);
		p.m_Friction = 0.82f;
		p.m_FlowAffected = 0.0f;
		p.m_Collides = false;
		p.m_Color = Color.WithAlpha(p.m_StartAlpha);
		GameClient()->m_Particles.Add(CParticles::GROUP_GENERAL, &p);
	}
}


