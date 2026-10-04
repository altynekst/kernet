#ifndef GAME_CLIENT_COMPONENTS_ZZ_HOOK_AIMBOT_H
#define GAME_CLIENT_COMPONENTS_ZZ_HOOK_AIMBOT_H

#include <array>

#include <base/vmath.h>

#include <engine/client/enums.h>
#include <engine/shared/protocol.h>

#include <game/client/component.h>

struct CNetObj_PlayerInput;

class CZZHookAimbot : public CComponent
{
	static constexpr int MAX_HOOK_PREDICTION_TICKS = 32;
	using THookPath = std::array<vec2, MAX_HOOK_PREDICTION_TICKS>;

	struct SHookPrediction
	{
		int m_Ticks = 1;
		std::array<THookPath, MAX_CLIENTS> m_aaPos{};
		std::array<std::array<bool, MAX_HOOK_PREDICTION_TICKS>, MAX_CLIENTS> m_aaActive{};
	};

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
	bool IsIgnoredFriend(int ClientId) const;
	bool BuildTarget(int ClientId, int DummyIndex, const vec2 &LocalPos,
		const vec2 &UserAim, float HalfFov, float HookLength,
		const SHookPrediction &Prediction, STarget &Target) const;
	void ClearLock(int DummyIndex);

public:
	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnStateChange(int NewState, int OldState) override;
	int LockedTarget(int DummyIndex) const
	{
		if(DummyIndex < 0 || DummyIndex >= NUM_DUMMIES)
			return -1;
		return m_aLockedTarget[DummyIndex];
	}

	bool Apply(CNetObj_PlayerInput *pInput, int DummyIndex, const vec2 &LocalPos,
		const vec2 &UserAim, bool InputBlocked, bool TeeOnly = false);
};

#endif
