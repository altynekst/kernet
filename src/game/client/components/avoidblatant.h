#ifndef GAME_CLIENT_COMPONENTS_AVOIDBLATANT_H
#define GAME_CLIENT_COMPONENTS_AVOIDBLATANT_H

#include <base/vmath.h>

#include <generated/protocol.h>

#include <game/client/component.h>

class CGameWorld;

// Predictive "avoid" assistant.
//
// Every packet the user's raw input is simulated a few ticks forward on a
// disposable copy of the predicted world. If that simulation ends in a
// hazard (freeze, death, optionally teleporters, or the void outside of the
// map) the component searches for the least intrusive replacement input
// (direction / jump / hook along the cursor) that survives the whole horizon.
// The module reacts only when the hazard is close (a "lead" of a few ticks that
// grows with ping), so the tee can still get right up to a freeze and squeeze
// through narrow gaps. If the user's input is safe nothing is touched. Everything runs
// synchronously on the main thread.
class CAvoidBlatant : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }

	void OnReset() override;
	void OnRender() override;

	// Called once per outgoing packet, after all other input helpers ran.
	// HigherPriorityActive is true when a module with a higher priority
	// (Laser unfreeze, Auto ALED) has already taken over this packet.
	// Returns true when the input was modified.
	bool OnPlayerInput(CNetObj_PlayerInput *pInput, bool HigherPriorityActive = false);

	bool IsOverriding() const { return m_Overriding; }

private:
	static constexpr int MAX_PATH_POINTS = 64;
	static constexpr int MAX_CANDIDATES = 24;

	enum EHookMode
	{
		HOOK_KEEP = 0,
		HOOK_RELEASE,
		HOOK_AIM,
		HOOK_MIRROR,
	};

	enum EState
	{
		STATE_IDLE = 0,
		STATE_DISABLED,
		STATE_YIELD,
		STATE_NOT_READY,
		STATE_SAFE,
		STATE_COMMIT,
		STATE_BEST_PLAN,
		STATE_GREEDY,
		STATE_NO_SOLUTION,
	};

	struct SOutcome
	{
		// First tick on which the tee is in danger, or Ticks + 1 if it never is.
		int m_Survive = 0;
		vec2 m_End = vec2(0.0f, 0.0f);
	};

	struct SCandidate
	{
		CNetObj_PlayerInput m_Input;
		int m_HookMode = HOOK_KEEP;
		int m_Cost = 0;
		SOutcome m_Outcome;
	};

	struct SHookAim
	{
		bool m_AimOk = false;
		bool m_MirrorOk = false;
		int m_AimX = 0;
		int m_AimY = 0;
		int m_MirrorX = 0;
		int m_MirrorY = 0;
	};

	// Result and cache of the last decision.
	bool m_Overriding = false;
	// Extra safety margin (pixels) added around the tee body; grows with ping.
	float m_Margin = 0.0f;
	int m_CachedTick = -1;
	bool m_CachedOverride = false;
	CNetObj_PlayerInput m_CachedRaw;
	CNetObj_PlayerInput m_CachedOut;

	// Commit: keep a chosen plan for a few ticks so the choice does not flip.
	bool m_HasCommit = false;
	int m_CommitUntil = -1;
	int m_CommitDirection = 0;
	int m_CommitJump = 0;
	int m_CommitHookMode = HOOK_KEEP;
	int m_GreedyCooldownUntil = -1;

	// Debug and visual data of the last evaluation.
	int m_State = STATE_IDLE;
	int m_DebugBaseSurvive = 0;
	int m_DebugPlanSurvive = 0;
	int m_DebugCandidates = 0;
	int m_DebugMicros = 0;
	int m_DebugPingTicks = 0;
	int m_DebugLead = 0;
	int m_DebugHookMode = HOOK_KEEP;
	int m_VisualTick = -1;
	vec2 m_aBasePath[MAX_PATH_POINTS];
	int m_NumBasePath = 0;
	int m_BaseHazardIndex = -1;
	vec2 m_aPlanPath[MAX_PATH_POINTS];
	int m_NumPlanPath = 0;

	void ClearCommit();
	bool Ready(int *pLocalId) const;

	bool IsHazardTile(int Tile) const;
	bool HazardAtPoint(vec2 Pos) const;
	bool HazardAtBody(vec2 Pos) const;
	int HazardProximity(vec2 Pos) const;

	bool FindHookHit(vec2 Pos, vec2 Dir) const;
	SHookAim EvaluateHookAim(const CNetObj_PlayerInput &Raw, int LocalId) const;

	bool StepWorld(CGameWorld &World, int LocalId, CNetObj_PlayerInput Input, vec2 *pPrev, vec2 *pCur) const;
	SOutcome Simulate(int LocalId, const CNetObj_PlayerInput &First, int FirstTicks,
		const CNetObj_PlayerInput &Follow, int Ticks, vec2 *pPath, int *pNumPath) const;
	int CandidateCost(const CNetObj_PlayerInput &Raw, const CNetObj_PlayerInput &Candidate, int HookMode) const;
	void MakeInput(const CNetObj_PlayerInput &Raw, int Direction, int Jump, int HookMode,
		const SHookAim &Hook, CNetObj_PlayerInput *pOut) const;
	int BuildCandidates(const CNetObj_PlayerInput &Raw, const SHookAim &Hook, SCandidate *pOut) const;
	bool GreedyRollout(int LocalId, const CNetObj_PlayerInput &Raw, const SHookAim &Hook, int Horizon,
		int64_t Deadline, CNetObj_PlayerInput *pFirst, int *pFirstHookMode) const;

	void ApplyPlan(CNetObj_PlayerInput *pInput, const CNetObj_PlayerInput &Plan) const;
};

#endif
