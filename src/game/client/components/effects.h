/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_EFFECTS_H
#define GAME_CLIENT_COMPONENTS_EFFECTS_H

#include <base/color.h>
#include <base/vmath.h>

#include <game/client/component.h>

class CEffects : public CComponent
{
private:
	bool m_Add5hz;
	int64_t m_LastUpdate5hz = 0;

	bool m_Add50hz;
	int64_t m_LastUpdate50hz = 0;

	bool m_Add100hz;
	int64_t m_LastUpdate100hz = 0;

	int64_t m_SkidSoundTimer = 0;
	float m_ZzQuality = 1.0f;

public:
	CEffects();
	int Sizeof() const override { return sizeof(*this); }

	void OnRender() override;

	void BulletTrail(vec2 Pos, float Alpha, float TimePassed);
	void SmokeTrail(vec2 Pos, vec2 Vel, float Alpha, float TimePassed);
	void SkidTrail(vec2 Pos, vec2 Vel, int Direction, float Alpha, float Volume);
	void Explosion(vec2 Pos, float Alpha);
	void HammerHit(vec2 Pos, float Alpha, float Volume);
	void AirJump(vec2 Pos, float Alpha, float Volume);
	void DamageIndicator(vec2 Pos, vec2 Dir, float Alpha);
	void PlayerSpawn(vec2 Pos, float Alpha, float Volume);
	void PlayerDeath(vec2 Pos, int ClientId, float Alpha);
	void PowerupShine(vec2 Pos, vec2 Size, float Alpha);
	void FreezingFlakes(vec2 Pos, vec2 Size, float Alpha);
	void SparkleTrail(vec2 Pos, float Alpha);
	void Confetti(vec2 Pos, float Alpha);
	void ZzLanding(vec2 Pos, float Speed, const ColorRGBA &Color, float Alpha);
	void ZzMotionTrail(vec2 Pos, vec2 Vel, const ColorRGBA &Color, float Alpha, bool OtherPlayer);
	void ZzFreezeVapor(vec2 Pos, float Alpha);
	void ZzFreezeBreak(vec2 Pos, float Alpha);
	void ZzWeaponBurst(vec2 Pos, vec2 Dir, int Weapon, float Alpha);
	void ZzPlayerPresence(vec2 Pos, const ColorRGBA &Color, bool Appear, float Alpha);
	ColorRGBA ZzWeaponColor(int Weapon) const;
	float ZzQuality() const { return m_ZzQuality; }

	void Update();
};
#endif
