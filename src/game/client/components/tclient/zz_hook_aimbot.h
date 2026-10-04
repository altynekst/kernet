#ifndef GAME_CLIENT_COMPONENTS_ZZ_HOOK_AIMBOT_H
#define GAME_CLIENT_COMPONENTS_ZZ_HOOK_AIMBOT_H

#include <base/vmath.h>

#include <engine/client/enums.h>

#include <game/client/component.h>

struct CNetObj_PlayerInput;

class CZZHookAimbot : public CComponent
{
	struct STarget
	{
		int m_ClientId = -1;
		vec2 m_AimDirection = vec2(1.0f, 0.0f);
		float m_Score = 0.0f;
		bool m_Frozen = false;

		bool Valid() const { return m_ClientId >= 0; }
	};

	int m_aLockedTarget[NUM_DUMMIES] = {-1, -1, -1};

	bool IsFrozen(int ClientId, int DummyIndex) const;
	bool BuildTarget(int ClientId, int DummyIndex, const vec2 &LocalPos, const vec2 &UserAim, float HalfFov, float HookLength, STarget &Target) const;
	void ClearLock(int DummyIndex);

public:
	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnStateChange(int NewState, int OldState) override;

	bool Apply(CNetObj_PlayerInput *pInput, int DummyIndex, const vec2 &LocalPos, const vec2 &UserAim, bool InputBlocked);
};

#endif
