#ifndef GAME_CLIENT_COMPONENTS_AVOIDBLATANT_H
#define GAME_CLIENT_COMPONENTS_AVOIDBLATANT_H

#include <base/vmath.h>

#include <generated/protocol.h>

#include <game/client/component.h>

#include <cstdint>
#include <vector>

class CGameWorld;

// Predictive "avoid" assistant.
//
// How it works, in short:
//  * Hazards (freeze, death, optionally teleporters, the void outside of the
//    map) are classified once per map into a per tile bit grid, so every test
//    inside the simulation is a couple of array reads.
//  * Every packet the user's raw input is simulated forward on a disposable copy
//    of the predicted world. If holding it is safe nothing is touched.
//  * Otherwise a viability check decides: the raw input is still allowed if,
//    after this tick, an escape plan exists. The tee may therefore walk right
//    up to a danger and slip through narrow gaps, but the step that would leave
//    no way out is never taken.
//  * A "safe distance" keeps the tee away from freeze in open space. It is
//    relaxed automatically where the tee is pinched between obstacles, so a one
//    tile gap can still be passed.
//  * When a rescue is needed the plan is chosen among all safe ones by its
//    clearance from the hazard and by how well it survives a one tick delay
//    (ping), then by how little it changes the user's input.
//
// Everything runs synchronously on the main thread with strict time budgets.
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
	static constexpr int MAX_HOOK_OPTIONS = 14;
	static constexpr int MAX_PER_TIER = 40;
	static constexpr int NUM_TIERS = 3;
	static constexpr int MAX_FULL_SAFE = 4;

	enum EHazardBits
	{
		HZ_FREEZE = 1,
		HZ_DEATH = 2,
		HZ_TELE = 4,
		HZ_SOLID = 8,
		HZ_ACTIVE_MASK = HZ_FREEZE | HZ_DEATH | HZ_TELE,
	};

	enum EHookMode
	{
		HOOK_KEEP = 0,
		HOOK_RELEASE,
		HOOK_AIM,
		HOOK_MIRROR,
		HOOK_FREE,
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
		STATE_VIABLE,
	};

	struct SOutcome
	{
		// First tick on which the tee is in danger, or Ticks + 1 if it never is.
		int m_Survive = 0;
		// Smallest clearance bucket along the path, higher is safer.
		int m_MinClear = 0;
		vec2 m_End = vec2(0.0f, 0.0f);
	};

	struct SCandidate
	{
		CNetObj_PlayerInput m_Input;
		int m_HookMode = HOOK_KEEP;
		int m_TargetX = 0;
		int m_TargetY = 0;
		int m_Cost = 0;
		bool m_IsCommit = false;
		SOutcome m_Outcome;
		int m_Robust = 0;
	};

	struct SHookOption
	{
		int m_Mode = HOOK_AIM;
		int m_X = 0;
		int m_Y = 0;
	};

	struct SHookSet
	{
		SHookOption m_aOptions[MAX_HOOK_OPTIONS];
		int m_Num = 0;
	};

	struct SContext
	{
		int m_LocalId = -1;
		int m_Tick = 0;
		vec2 m_StartPos = vec2(0.0f, 0.0f);
		int m_Horizon = 0;
		int m_Commit = 1;
		int64_t m_ViabilityDeadline = 0;
		int64_t m_HardDeadline = 0;
		CNetObj_PlayerInput m_Raw;
		SHookSet m_Hooks;
		SCandidate m_aTier[NUM_TIERS][MAX_PER_TIER];
		int m_aNumTier[NUM_TIERS] = {0, 0, 0};
	};

	struct SDecision
	{
		bool m_Override = false;
		bool m_Resolved = false;
		CNetObj_PlayerInput m_Plan;
		int m_HookMode = HOOK_KEEP;
		int m_TargetX = 0;
		int m_TargetY = 0;
		int m_Survive = 0;
		int m_State = STATE_IDLE;
	};

	// Result and cache of the last decision.
	bool m_Overriding = false;
	int m_CachedTick = -1;
	bool m_CachedOverride = false;
	CNetObj_PlayerInput m_CachedRaw;
	CNetObj_PlayerInput m_CachedOut;

	// Hazard grid, built once per map in small time slices.
	std::vector<uint8_t> m_vHazard;
	int m_GridWidth = 0;
	int m_GridHeight = 0;
	int m_GridBuilt = 0;
	const void *m_pGridKey = nullptr;

	// Per decision settings.
	float m_PingMargin = 0.0f;
	bool m_SoftEnabled = false;

	// Commit: keep a chosen plan for a few ticks so the choice does not flip.
	bool m_HasCommit = false;
	int m_CommitUntil = -1;
	int m_CommitDirection = 0;
	int m_CommitJump = 0;
	int m_CommitHookMode = HOOK_KEEP;
	int m_CommitTargetX = 0;
	int m_CommitTargetY = 0;
	int m_GreedyCooldownUntil = -1;

	// The escape plan that made the last raw input "viable". It is offered again
	// as a candidate on the next ticks, so a rescue that existed one tick ago
	// cannot silently disappear.
	bool m_HasHint = false;
	int m_HintUntil = -1;
	int m_HintDirection = 0;
	int m_HintJump = 0;
	int m_HintHookMode = HOOK_KEEP;
	int m_HintTargetX = 0;
	int m_HintTargetY = 0;

	// Debug and visual data of the last evaluation.
	int m_State = STATE_IDLE;
	int m_DebugBaseSurvive = 0;
	int m_DebugPlanSurvive = 0;
	int m_DebugCandidates = 0;
	int m_DebugMicros = 0;
	int m_DebugPingTicks = 0;
	int m_DebugHorizon = 0;
	int m_DebugHookMode = HOOK_KEEP;
	int m_DebugTier = 0;
	int m_DebugClear = 0;
	bool m_DebugSoft = false;
	int m_VisualTick = -1;
	vec2 m_aBasePath[MAX_PATH_POINTS];
	int m_NumBasePath = 0;
	int m_BaseHazardIndex = -1;
	vec2 m_aPlanPath[MAX_PATH_POINTS];
	int m_NumPlanPath = 0;

	void ClearCommit();
	bool Ready(int *pLocalId) const;

	// Hazard grid and tests.
	int ComputeTileMask(int Index) const;
	bool EnsureGrid();
	int ActiveMask() const;
	int TileMaskAt(int Nx, int Ny) const;
	bool RectHazard(vec2 Pos, float Hx, float Hy) const;
	bool OutOfMap(vec2 Pos) const;
	float PinchMargin(bool Horizontal, int Line, vec2 Pos, float Wanted) const;
	bool SoftViolation(vec2 Pos) const;
	bool BodyUnsafe(vec2 Pos) const;
	int ClearanceBucket(vec2 Pos) const;

	// Hook: along the cursor, opposite to it, or (optionally) any verified direction.
	bool FindHookHit(vec2 Pos, vec2 Dir) const;
	void EvaluateHooks(const CNetObj_PlayerInput &Raw, int LocalId, SHookSet *pOut) const;

	// Simulation.
	bool StepWorld(CGameWorld &World, int LocalId, CNetObj_PlayerInput Input, vec2 *pPrev, vec2 *pCur) const;
	SOutcome Simulate(int LocalId, const CNetObj_PlayerInput &First, int FirstTicks,
		const CNetObj_PlayerInput &Follow, int Ticks, vec2 *pPath, int *pNumPath, bool WantClearance) const;
	bool PolicySurvives(CGameWorld &Start, int LocalId, const SCandidate &Policy, int Ticks) const;
	bool DelayedSurvives(int LocalId, const CNetObj_PlayerInput &Raw, const SCandidate &Plan,
		int Horizon) const;
	bool GreedyRollout(CGameWorld &Rolling, int LocalId, const CNetObj_PlayerInput &Raw, const SHookSet &Hooks,
		int Horizon, int64_t Deadline, CNetObj_PlayerInput *pFirst, int *pFirstHookMode, bool RequireStable) const;
	int ComputeHorizon(int LocalId, int PingTicks) const;
	int BallisticImpactTick(vec2 Pos, vec2 Vel) const;
	bool TerminalSafe(CGameWorld &World, int LocalId) const;

	// Decision making.
	void MakeInput(const CNetObj_PlayerInput &Raw, int Direction, int Jump, int HookMode, int TargetX,
		int TargetY, CNetObj_PlayerInput *pOut) const;
	int CandidateCost(const CNetObj_PlayerInput &Raw, const CNetObj_PlayerInput &Candidate, int HookMode) const;
	void BuildTiers(SContext &Context) const;
	bool RawIsViable(SContext &Context, SCandidate *pWinner) const;
	SDecision Decide(SContext &Context);
	void ApplyPlan(CNetObj_PlayerInput *pInput, const CNetObj_PlayerInput &Plan) const;
};

#endif
