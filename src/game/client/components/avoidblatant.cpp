#include "avoidblatant.h"

#include <base/system.h>

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <game/client/components/controls.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>
#include <game/collision.h>
#include <game/mapitems.h>

#include <algorithm>
#include <climits>
#include <cmath>

namespace
{
	constexpr float TILE_SIZE = 32.0f;
	// The tee body is sampled in the center and at four diagonal offsets, which
	// is close to how the server checks the tiles around a character.
	constexpr float BODY_OFFSET = 9.0f;
	constexpr float SEGMENT_STEP = 6.0f;

	const char *StateName(int State)
	{
		switch(State)
		{
		case 1: return "off";
		case 2: return "yield";
		case 3: return "not ready";
		case 4: return "safe";
		case 5: return "commit";
		case 6: return "plan";
		case 7: return "greedy";
		case 8: return "no solution";
		default: return "idle";
		}
	}

	const char *HookName(int Mode)
	{
		switch(Mode)
		{
		case 1: return "release";
		case 2: return "hook aim";
		case 3: return "hook mirror";
		default: return "-";
		}
	}
} // namespace

// ----------------------------------------------------------------------------
// Lifecycle
// ----------------------------------------------------------------------------

void CAvoidBlatant::OnReset()
{
	ClearCommit();
	m_Overriding = false;
	m_CachedTick = -1;
	m_CachedOverride = false;
	m_GreedyCooldownUntil = -1;
	m_State = STATE_IDLE;
	m_VisualTick = -1;
	m_NumBasePath = 0;
	m_NumPlanPath = 0;
	m_BaseHazardIndex = -1;
}

void CAvoidBlatant::ClearCommit()
{
	m_HasCommit = false;
	m_CommitUntil = -1;
	m_CommitDirection = 0;
	m_CommitJump = 0;
	m_CommitHookMode = HOOK_KEEP;
}

bool CAvoidBlatant::Ready(int *pLocalId) const
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return false;
	if(g_Config.m_ClDummy < 0 || g_Config.m_ClDummy >= NUM_DUMMIES)
		return false;
	const CCollision *pCollision = Collision();
	if(!pCollision || pCollision->GetWidth() <= 0 || pCollision->GetHeight() <= 0)
		return false;
	if(!GameClient()->m_Snap.m_pLocalCharacter)
		return false;
	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	if(LocalId < 0 || LocalId >= MAX_CLIENTS)
		return false;
	if(!GameClient()->m_Snap.m_aCharacters[LocalId].m_Active)
		return false;
	CCharacter *pChar = GameClient()->m_RegularPredictedWorld.GetCharacterById(LocalId);
	if(!pChar)
		return false;
	// A frozen tee cannot act, there is nothing to correct.
	if(pChar->m_FreezeTime > 0)
		return false;
	*pLocalId = LocalId;
	return true;
}

// ----------------------------------------------------------------------------
// Hazard detection
// ----------------------------------------------------------------------------

bool CAvoidBlatant::IsHazardTile(int Tile) const
{
	if(g_Config.m_AvoidBlatantFreeze &&
		(Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE))
		return true;
	if(g_Config.m_AvoidBlatantDeath && Tile == TILE_DEATH)
		return true;
	if(g_Config.m_AvoidBlatantTele &&
		(Tile == TILE_TELEIN || Tile == TILE_TELEINEVIL ||
			Tile == TILE_TELEINHOOK || Tile == TILE_TELEINWEAPON))
		return true;
	return false;
}

bool CAvoidBlatant::HazardAtPoint(vec2 Pos) const
{
	const CCollision *pCollision = Collision();
	const int Width = pCollision->GetWidth();
	const int Height = pCollision->GetHeight();
	// The void behind the edge of the map is deadly, treat it like a death tile.
	if(g_Config.m_AvoidBlatantDeath &&
		(Pos.x < 0.0f || Pos.y < 0.0f || Pos.x >= Width * TILE_SIZE || Pos.y >= Height * TILE_SIZE))
		return true;
	const int Nx = std::clamp((int)(Pos.x / TILE_SIZE), 0, Width - 1);
	const int Ny = std::clamp((int)(Pos.y / TILE_SIZE), 0, Height - 1);
	const int Index = Ny * Width + Nx;
	if(IsHazardTile(pCollision->GetTileIndex(Index)) ||
		IsHazardTile(pCollision->GetFrontTileIndex(Index)))
		return true;
	if(pCollision->SwitchLayer() && IsHazardTile(pCollision->GetSwitchType(Index)))
		return true;
	return false;
}

bool CAvoidBlatant::HazardAtBody(vec2 Pos) const
{
	if(HazardAtPoint(Pos))
		return true;
	return HazardAtPoint(Pos + vec2(BODY_OFFSET, BODY_OFFSET)) ||
	       HazardAtPoint(Pos + vec2(-BODY_OFFSET, BODY_OFFSET)) ||
	       HazardAtPoint(Pos + vec2(BODY_OFFSET, -BODY_OFFSET)) ||
	       HazardAtPoint(Pos + vec2(-BODY_OFFSET, -BODY_OFFSET));
}

bool CAvoidBlatant::HazardOnSegment(vec2 From, vec2 To) const
{
	const float Length = distance(From, To);
	const int Steps = std::max(1, (int)(Length / SEGMENT_STEP));
	for(int Step = 1; Step <= Steps; ++Step)
	{
		if(HazardAtBody(mix(From, To, (float)Step / (float)Steps)))
			return true;
	}
	return false;
}

int CAvoidBlatant::HazardProximity(vec2 Pos) const
{
	const CCollision *pCollision = Collision();
	const int Width = pCollision->GetWidth();
	const int Height = pCollision->GetHeight();
	const int CenterX = (int)(Pos.x / TILE_SIZE);
	const int CenterY = (int)(Pos.y / TILE_SIZE);
	int Count = 0;
	for(int Y = CenterY - 2; Y <= CenterY + 2; ++Y)
	{
		for(int X = CenterX - 2; X <= CenterX + 2; ++X)
		{
			if(X < 0 || Y < 0 || X >= Width || Y >= Height)
				continue;
			const int Index = Y * Width + X;
			if(IsHazardTile(pCollision->GetTileIndex(Index)) ||
				IsHazardTile(pCollision->GetFrontTileIndex(Index)))
				++Count;
		}
	}
	return Count;
}

// ----------------------------------------------------------------------------
// Hook: strictly along the cursor, only if something hookable is hit
// ----------------------------------------------------------------------------

bool CAvoidBlatant::FindHookHit(vec2 Pos, vec2 Dir) const
{
	if(length(Dir) < 0.001f)
		return false;
	const float HookLength = (float)GameClient()->m_aTuning[g_Config.m_ClDummy].m_HookLength;
	const vec2 End = Pos + normalize(Dir) * HookLength;
	vec2 HitPos;
	vec2 BeforeHit;
	const int Hit = Collision()->IntersectLine(Pos, End, &HitPos, &BeforeHit);
	// Nothing hit (void, open air) or an unhookable tile: do not hook at all.
	if(Hit == 0 || Hit == TILE_NOHOOK)
		return false;
	return distance(Pos, HitPos) > CCharacterCore::PhysicalSize();
}

CAvoidBlatant::SHookAim CAvoidBlatant::EvaluateHookAim(const CNetObj_PlayerInput &Raw, int LocalId) const
{
	SHookAim Result;
	if(!g_Config.m_AvoidBlatantHookAssist)
		return Result;
	CCharacter *pChar = GameClient()->m_RegularPredictedWorld.GetCharacterById(LocalId);
	if(!pChar)
		return Result;
	vec2 Aim = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];
	if(length(Aim) < 1.0f)
		Aim = vec2((float)Raw.m_TargetX, (float)Raw.m_TargetY);
	if(length(Aim) < 1.0f)
		return Result;
	const vec2 Pos = pChar->GetPos();
	const vec2 Dir = normalize(Aim);
	Result.m_AimOk = FindHookHit(Pos, Dir);
	Result.m_AimX = round_to_int(Dir.x * 256.0f);
	Result.m_AimY = round_to_int(Dir.y * 256.0f);
	// If the cursor points into danger the hook goes to the opposite side.
	Result.m_MirrorOk = FindHookHit(Pos, -Dir);
	Result.m_MirrorX = -Result.m_AimX;
	Result.m_MirrorY = -Result.m_AimY;
	return Result;
}

// ----------------------------------------------------------------------------
// Simulation
// ----------------------------------------------------------------------------

bool CAvoidBlatant::StepWorld(CGameWorld &World, int LocalId, CNetObj_PlayerInput Input,
	vec2 *pPrev, vec2 *pCur) const
{
	CCharacter *pChar = World.GetCharacterById(LocalId);
	if(!pChar)
		return false;
	if(Input.m_TargetX == 0 && Input.m_TargetY == 0)
		Input.m_TargetY = -1;
	*pPrev = pChar->GetPos();
	pChar->OnDirectInput(&Input);
	World.m_GameTick++;
	pChar->OnPredictedInput(&Input);
	World.Tick();
	pChar = World.GetCharacterById(LocalId);
	if(!pChar)
		return false;
	*pCur = pChar->GetPos();
	if(g_Config.m_AvoidBlatantFreeze && (pChar->Core()->m_IsInFreeze || pChar->m_FreezeTime > 0))
		return false;
	return !HazardOnSegment(*pPrev, *pCur);
}

CAvoidBlatant::SOutcome CAvoidBlatant::Simulate(int LocalId, const CNetObj_PlayerInput &First, int FirstTicks,
	const CNetObj_PlayerInput &Follow, int Ticks, vec2 *pPath, int *pNumPath) const
{
	SOutcome Result;
	Result.m_Survive = Ticks + 1;

	CGameWorld SimWorld;
	SimWorld.CopyWorldClean(&GameClient()->m_RegularPredictedWorld);
	SimWorld.m_WorldConfig.m_PredictFreeze = 1;
	CCharacter *pChar = SimWorld.GetCharacterById(LocalId);
	if(!pChar)
	{
		Result.m_Survive = 0;
		return Result;
	}
	Result.m_End = pChar->GetPos();
	int PathCount = 0;
	if(pPath)
		pPath[PathCount++] = Result.m_End;

	for(int Tick = 1; Tick <= Ticks; ++Tick)
	{
		vec2 Prev;
		vec2 Cur;
		const bool Safe = StepWorld(SimWorld, LocalId, Tick <= FirstTicks ? First : Follow, &Prev, &Cur);
		CCharacter *pAfter = SimWorld.GetCharacterById(LocalId);
		if(pAfter)
			Result.m_End = pAfter->GetPos();
		if(pPath && PathCount < MAX_PATH_POINTS)
			pPath[PathCount++] = Result.m_End;
		if(!Safe)
		{
			Result.m_Survive = Tick;
			break;
		}
	}
	if(pNumPath)
		*pNumPath = PathCount;
	return Result;
}

// ----------------------------------------------------------------------------
// Candidate generation and selection
// ----------------------------------------------------------------------------

int CAvoidBlatant::CandidateCost(const CNetObj_PlayerInput &Raw, const CNetObj_PlayerInput &Candidate,
	int HookMode) const
{
	int Cost = 0;
	if(Candidate.m_Direction != Raw.m_Direction)
	{
		// Releasing the key is the gentlest change, reversing the strongest.
		Cost += (Raw.m_Direction != 0 && Candidate.m_Direction == 0) ? 1 : 2;
	}
	if((Candidate.m_Jump & 1) != (Raw.m_Jump & 1))
		Cost += 1;
	if(HookMode == HOOK_RELEASE)
		Cost += 1;
	else if(HookMode == HOOK_AIM)
		Cost += 3;
	else if(HookMode == HOOK_MIRROR)
		Cost += 4;
	return Cost;
}

void CAvoidBlatant::MakeInput(const CNetObj_PlayerInput &Raw, int Direction, int Jump, int HookMode,
	const SHookAim &Hook, CNetObj_PlayerInput *pOut) const
{
	*pOut = Raw;
	pOut->m_Direction = Direction;
	pOut->m_Jump = Jump;
	if(HookMode == HOOK_RELEASE)
		pOut->m_Hook = 0;
	else if(HookMode == HOOK_AIM)
	{
		pOut->m_Hook = 1;
		pOut->m_TargetX = Hook.m_AimX;
		pOut->m_TargetY = Hook.m_AimY;
	}
	else if(HookMode == HOOK_MIRROR)
	{
		pOut->m_Hook = 1;
		pOut->m_TargetX = Hook.m_MirrorX;
		pOut->m_TargetY = Hook.m_MirrorY;
	}
}

int CAvoidBlatant::BuildCandidates(const CNetObj_PlayerInput &Raw, const SHookAim &Hook, SCandidate *pOut) const
{
	int Count = 0;
	const bool CanJump = (Raw.m_Jump & 1) == 0;
	const int aJumps[2] = {Raw.m_Jump, 1};
	const int NumJumps = CanJump ? 2 : 1;

	int aHookModes[3];
	int NumHookModes = 0;
	aHookModes[NumHookModes++] = HOOK_KEEP;
	if(Raw.m_Hook)
		aHookModes[NumHookModes++] = HOOK_RELEASE;
	else
	{
		if(Hook.m_AimOk)
			aHookModes[NumHookModes++] = HOOK_AIM;
		if(Hook.m_MirrorOk)
			aHookModes[NumHookModes++] = HOOK_MIRROR;
	}

	for(int Direction = -1; Direction <= 1; ++Direction)
	{
		for(int JumpIndex = 0; JumpIndex < NumJumps; ++JumpIndex)
		{
			for(int HookIndex = 0; HookIndex < NumHookModes; ++HookIndex)
			{
				const int HookMode = aHookModes[HookIndex];
				if(Direction == Raw.m_Direction && aJumps[JumpIndex] == Raw.m_Jump && HookMode == HOOK_KEEP)
					continue; // identical to the raw input
				if(Count >= MAX_CANDIDATES)
					return Count;
				SCandidate &Candidate = pOut[Count++];
				MakeInput(Raw, Direction, aJumps[JumpIndex], HookMode, Hook, &Candidate.m_Input);
				Candidate.m_HookMode = HookMode;
				Candidate.m_Cost = CandidateCost(Raw, Candidate.m_Input, HookMode);
			}
		}
	}
	return Count;
}

bool CAvoidBlatant::GreedyRollout(int LocalId, const CNetObj_PlayerInput &Raw, const SHookAim &Hook,
	int Horizon, int64_t Deadline, CNetObj_PlayerInput *pFirst, int *pFirstHookMode) const
{
	struct SAction
	{
		int m_Direction;
		int m_Jump;
		int m_HookMode;
	};
	SAction aActions[12];
	int NumActions = 0;
	const bool CanJump = (Raw.m_Jump & 1) == 0;
	const int aJumps[2] = {Raw.m_Jump, 1};
	const int NumJumps = CanJump ? 2 : 1;
	for(int Direction = -1; Direction <= 1; ++Direction)
	{
		for(int JumpIndex = 0; JumpIndex < NumJumps; ++JumpIndex)
			aActions[NumActions++] = {Direction, aJumps[JumpIndex], HOOK_KEEP};
		if(!Raw.m_Hook && Hook.m_AimOk)
			aActions[NumActions++] = {Direction, Raw.m_Jump, HOOK_AIM};
	}

	CGameWorld Rolling;
	Rolling.CopyWorldClean(&GameClient()->m_RegularPredictedWorld);
	Rolling.m_WorldConfig.m_PredictFreeze = 1;

	for(int Tick = 1; Tick <= Horizon; ++Tick)
	{
		int BestIndex = -1;
		int BestScore = INT_MIN;
		for(int Index = 0; Index < NumActions; ++Index)
		{
			if(time_get() > Deadline)
				return false;
			CNetObj_PlayerInput Input;
			MakeInput(Raw, aActions[Index].m_Direction, aActions[Index].m_Jump,
				aActions[Index].m_HookMode, Hook, &Input);

			CGameWorld Trial;
			Trial.CopyWorldClean(&Rolling);
			Trial.m_WorldConfig.m_PredictFreeze = 1;
			vec2 Prev;
			vec2 Cur;
			if(!StepWorld(Trial, LocalId, Input, &Prev, &Cur))
				continue;
			// Short probe: the same action must not run straight into danger.
			bool ProbeOk = true;
			for(int Probe = 0; Probe < 2 && ProbeOk; ++Probe)
				ProbeOk = StepWorld(Trial, LocalId, Input, &Prev, &Cur);
			if(!ProbeOk)
				continue;
			const int Score = -HazardProximity(Cur) * 4 -
					  CandidateCost(Raw, Input, aActions[Index].m_HookMode);
			if(Score > BestScore)
			{
				BestScore = Score;
				BestIndex = Index;
			}
		}
		if(BestIndex < 0)
			return false;

		CNetObj_PlayerInput Chosen;
		MakeInput(Raw, aActions[BestIndex].m_Direction, aActions[BestIndex].m_Jump,
			aActions[BestIndex].m_HookMode, Hook, &Chosen);
		vec2 Prev;
		vec2 Cur;
		if(!StepWorld(Rolling, LocalId, Chosen, &Prev, &Cur))
			return false;
		if(Tick == 1)
		{
			*pFirst = Chosen;
			*pFirstHookMode = aActions[BestIndex].m_HookMode;
		}
	}
	return true;
}

void CAvoidBlatant::ApplyPlan(CNetObj_PlayerInput *pInput, const CNetObj_PlayerInput &Plan) const
{
	// Only movement fields are touched. Fire and weapon selection are never
	// changed by this module.
	pInput->m_Direction = Plan.m_Direction;
	pInput->m_Jump = Plan.m_Jump;
	pInput->m_Hook = Plan.m_Hook;
	pInput->m_TargetX = Plan.m_TargetX;
	pInput->m_TargetY = Plan.m_TargetY;
}

// ----------------------------------------------------------------------------
// Main entry point
// ----------------------------------------------------------------------------

bool CAvoidBlatant::OnPlayerInput(CNetObj_PlayerInput *pInput, bool HigherPriorityActive)
{
	m_Overriding = false;
	if(!pInput)
		return false;
	if(!g_Config.m_AvoidBlatantEnabled)
	{
		m_State = STATE_DISABLED;
		ClearCommit();
		m_NumBasePath = 0;
		m_NumPlanPath = 0;
		return false;
	}
	// Priority: Laser unfreeze > Auto ALED > Avoid blatant.
	if(HigherPriorityActive || GameClient()->m_LaserUnfreeze.m_Claimed)
	{
		m_State = STATE_YIELD;
		ClearCommit();
		return false;
	}
	int LocalId = -1;
	if(!Ready(&LocalId))
	{
		m_State = STATE_NOT_READY;
		ClearCommit();
		return false;
	}

	const int Tick = Client()->PredGameTick(g_Config.m_ClDummy);
	if(Tick == m_CachedTick && mem_comp(&m_CachedRaw, pInput, sizeof(*pInput)) == 0)
	{
		if(m_CachedOverride)
		{
			*pInput = m_CachedOut;
			m_Overriding = true;
		}
		return m_CachedOverride;
	}

	const int64_t StartTime = time_get();
	const CNetObj_PlayerInput Raw = *pInput;
	const int Predict = std::clamp(g_Config.m_AvoidBlatantPredictTicks, 1, 60);
	const int Commit = std::clamp(g_Config.m_AvoidBlatantCommitTicks, 1, 20);

	m_NumBasePath = 0;
	m_NumPlanPath = 0;
	const SOutcome Base = Simulate(LocalId, Raw, Predict, Raw, Predict, m_aBasePath, &m_NumBasePath);
	m_BaseHazardIndex = Base.m_Survive <= Predict ? Base.m_Survive : -1;
	m_VisualTick = Tick;
	m_DebugBaseSurvive = Base.m_Survive;
	m_DebugPlanSurvive = Base.m_Survive;
	m_DebugCandidates = 0;
	m_DebugHookMode = HOOK_KEEP;

	bool Override = false;
	CNetObj_PlayerInput Plan = Raw;
	int PlanHookMode = HOOK_KEEP;
	int PlanSurvive = Base.m_Survive;
	int State = STATE_SAFE;

	if(Base.m_Survive > Predict)
	{
		// The player is safe on their own: do not interfere.
		ClearCommit();
	}
	else
	{
		const SHookAim Hook = EvaluateHookAim(Raw, LocalId);
		SCandidate aCandidates[MAX_CANDIDATES + 1];
		int NumCandidates = BuildCandidates(Raw, Hook, aCandidates);

		// The plan chosen on the previous tick competes with a small bonus so
		// the decision does not flip between equal alternatives.
		bool CommitIndexValid = false;
		int CommitIndex = -1;
		if(m_HasCommit && Tick <= m_CommitUntil && NumCandidates < MAX_CANDIDATES + 1)
		{
			const bool NeedsAim = m_CommitHookMode == HOOK_AIM;
			const bool NeedsMirror = m_CommitHookMode == HOOK_MIRROR;
			if((!NeedsAim || Hook.m_AimOk) && (!NeedsMirror || Hook.m_MirrorOk))
			{
				SCandidate &Candidate = aCandidates[NumCandidates];
				MakeInput(Raw, m_CommitDirection, m_CommitJump, m_CommitHookMode, Hook, &Candidate.m_Input);
				Candidate.m_HookMode = m_CommitHookMode;
				Candidate.m_Cost = CandidateCost(Raw, Candidate.m_Input, m_CommitHookMode) - 1;
				CommitIndex = NumCandidates;
				CommitIndexValid = true;
				++NumCandidates;
			}
		}
		m_DebugCandidates = NumCandidates;

		int BestIndex = -1;
		for(int Index = 0; Index < NumCandidates; ++Index)
		{
			SCandidate &Candidate = aCandidates[Index];
			CNetObj_PlayerInput Follow = Candidate.m_Input;
			int FirstTicks = Predict;
			if(Candidate.m_HookMode == HOOK_AIM || Candidate.m_HookMode == HOOK_MIRROR)
			{
				// A hook assist holds the hook only for the commit window.
				Follow.m_Hook = 0;
				FirstTicks = Commit;
			}
			Candidate.m_Outcome = Simulate(LocalId, Candidate.m_Input, FirstTicks, Follow, Predict, nullptr, nullptr);
			if(BestIndex < 0 ||
				Candidate.m_Outcome.m_Survive > aCandidates[BestIndex].m_Outcome.m_Survive ||
				(Candidate.m_Outcome.m_Survive == aCandidates[BestIndex].m_Outcome.m_Survive &&
					Candidate.m_Cost < aCandidates[BestIndex].m_Cost))
				BestIndex = Index;
		}

		if(BestIndex >= 0 && aCandidates[BestIndex].m_Outcome.m_Survive > Predict)
		{
			Override = true;
			Plan = aCandidates[BestIndex].m_Input;
			PlanHookMode = aCandidates[BestIndex].m_HookMode;
			PlanSurvive = aCandidates[BestIndex].m_Outcome.m_Survive;
			State = (CommitIndexValid && BestIndex == CommitIndex) ? STATE_COMMIT : STATE_BEST_PLAN;
		}
		else
		{
			// Nothing saves the tee outright: greedy rollout, which picks the
			// best single action on every tick of the horizon.
			bool GreedyOk = false;
			if(Tick >= m_GreedyCooldownUntil)
			{
				const int64_t Deadline = time_get() + time_freq() * 3 / 1000;
				CNetObj_PlayerInput First = Raw;
				int FirstHookMode = HOOK_KEEP;
				GreedyOk = GreedyRollout(LocalId, Raw, Hook, std::min(Predict, 12), Deadline, &First, &FirstHookMode);
				if(GreedyOk)
				{
					Override = true;
					Plan = First;
					PlanHookMode = FirstHookMode;
					PlanSurvive = Predict + 1;
					State = STATE_GREEDY;
				}
				else
					m_GreedyCooldownUntil = Tick + 5;
			}
			if(!GreedyOk)
			{
				// Still take a plan that at least delays the hazard.
				if(BestIndex >= 0 && aCandidates[BestIndex].m_Outcome.m_Survive > Base.m_Survive)
				{
					Override = true;
					Plan = aCandidates[BestIndex].m_Input;
					PlanHookMode = aCandidates[BestIndex].m_HookMode;
					PlanSurvive = aCandidates[BestIndex].m_Outcome.m_Survive;
					State = STATE_BEST_PLAN;
				}
				else
					State = STATE_NO_SOLUTION;
			}
		}

		if(Override)
		{
			m_HasCommit = true;
			m_CommitUntil = Tick + Commit;
			m_CommitDirection = Plan.m_Direction;
			m_CommitJump = Plan.m_Jump;
			m_CommitHookMode = PlanHookMode;
			if(g_Config.m_AvoidBlatantVisualPath)
			{
				CNetObj_PlayerInput Follow = Plan;
				int FirstTicks = Predict;
				if(PlanHookMode == HOOK_AIM || PlanHookMode == HOOK_MIRROR)
				{
					Follow.m_Hook = 0;
					FirstTicks = Commit;
				}
				Simulate(LocalId, Plan, FirstTicks, Follow, Predict, m_aPlanPath, &m_NumPlanPath);
			}
		}
		else
			ClearCommit();
	}

	m_State = State;
	m_DebugPlanSurvive = PlanSurvive;
	m_DebugHookMode = PlanHookMode;
	if(Override)
	{
		ApplyPlan(pInput, Plan);
		m_Overriding = true;
	}
	const int64_t Elapsed = time_get() - StartTime;
	m_DebugMicros = (int)(Elapsed * 1000000 / std::max<int64_t>(time_freq(), 1));

	m_CachedTick = Tick;
	m_CachedRaw = Raw;
	m_CachedOverride = Override;
	m_CachedOut = *pInput;
	return Override;
}

// ----------------------------------------------------------------------------
// Visualisation
// ----------------------------------------------------------------------------

void CAvoidBlatant::OnRender()
{
	if(!g_Config.m_AvoidBlatantEnabled)
		return;
	if(!g_Config.m_AvoidBlatantVisualPath && !g_Config.m_AvoidBlatantDebug)
		return;
	if(Client()->State() != IClient::STATE_ONLINE || m_VisualTick < 0)
		return;
	const int Now = Client()->PredGameTick(g_Config.m_ClDummy);
	if(Now - m_VisualTick > 25 || m_NumBasePath < 1)
		return;

	if(g_Config.m_AvoidBlatantVisualPath)
	{
		Graphics()->TextureClear();
		Graphics()->LinesBegin();
		for(int Index = 1; Index < m_NumBasePath; ++Index)
		{
			const bool Bad = m_BaseHazardIndex >= 0 && Index >= m_BaseHazardIndex;
			if(Bad)
				Graphics()->SetColor(1.0f, 0.25f, 0.25f, 0.95f);
			else
				Graphics()->SetColor(0.30f, 1.0f, 0.45f, 0.80f);
			const IGraphics::CLineItem Line(m_aBasePath[Index - 1], m_aBasePath[Index]);
			Graphics()->LinesDraw(&Line, 1);
		}
		Graphics()->SetColor(0.30f, 0.90f, 1.0f, 0.95f);
		for(int Index = 1; Index < m_NumPlanPath; ++Index)
		{
			const IGraphics::CLineItem Line(m_aPlanPath[Index - 1], m_aPlanPath[Index]);
			Graphics()->LinesDraw(&Line, 1);
		}
		Graphics()->LinesEnd();

		if(m_BaseHazardIndex >= 0 && m_BaseHazardIndex < m_NumBasePath)
		{
			const vec2 Point = m_aBasePath[m_BaseHazardIndex];
			Graphics()->DrawRect(Point.x - 4.0f, Point.y - 4.0f, 8.0f, 8.0f,
				ColorRGBA(1.0f, 0.2f, 0.2f, 0.9f), IGraphics::CORNER_ALL, 4.0f);
		}
	}

	if(g_Config.m_AvoidBlatantDebug)
	{
		char aBuf[160];
		str_format(aBuf, sizeof(aBuf), "Avoid: %s | base %d | plan %d | cand %d | %s | %d us",
			StateName(m_State), m_DebugBaseSurvive, m_DebugPlanSurvive, m_DebugCandidates,
			HookName(m_DebugHookMode), m_DebugMicros);
		const vec2 Origin = m_aBasePath[0];
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.95f);
		TextRender()->Text(Origin.x - 90.0f, Origin.y - 80.0f, 14.0f, aBuf);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	}
}
