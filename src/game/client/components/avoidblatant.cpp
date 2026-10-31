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
#include <game/layers.h>
#include <game/mapitems.h>

#include <algorithm>
#include <climits>
#include <cmath>

namespace
{
	constexpr float TILE_SIZE = 32.0f;
	// The server checks the tile under the center of the tee and under four
	// corners at PhysicalSize / 3. The same points are used here, and only at the
	// end of every tick like the server does, so a one tile gap can be passed.
	constexpr float BODY_OFFSET = CCharacterCore::PhysicalSize() / 3.0f;
	// Free play (pixels) that is always left on each side of the tee when the
	// safe distance is relaxed inside a narrow passage.
	constexpr float NARROW_PLAY = 5.5f;
	constexpr int SCAN_TILES = 3;
	constexpr int NUM_CLEARANCE_STEPS = 6;
	constexpr float CLEARANCE_STEPS[NUM_CLEARANCE_STEPS] = {0.0f, 6.0f, 12.0f, 20.0f, 32.0f, 48.0f};
	constexpr int MAX_CLEARANCE = NUM_CLEARANCE_STEPS;

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
		case 9: return "viable";
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
		case 4: return "hook free";
		default: return "-";
		}
	}

	bool IsFreezeTile(int Tile)
	{
		return Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE;
	}

	bool IsTeleTile(int Tile)
	{
		return Tile == TILE_TELEIN || Tile == TILE_TELEINEVIL ||
		       Tile == TILE_TELEINHOOK || Tile == TILE_TELEINWEAPON;
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
	m_HasHint = false;
	m_State = STATE_IDLE;
	m_VisualTick = -1;
	m_NumBasePath = 0;
	m_NumPlanPath = 0;
	m_BaseHazardIndex = -1;
	// A new map invalidates the hazard grid.
	m_pGridKey = nullptr;
	m_GridBuilt = 0;
}

void CAvoidBlatant::ClearCommit()
{
	m_HasCommit = false;
	m_CommitUntil = -1;
	m_CommitDirection = 0;
	m_CommitJump = 0;
	m_CommitHookMode = HOOK_KEEP;
	m_CommitTargetX = 0;
	m_CommitTargetY = 0;
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
	// A frozen tee cannot act, and a super or invincible tee cannot be frozen.
	if(pChar->m_FreezeTime > 0 || pChar->Core()->m_DeepFrozen)
		return false;
	if(pChar->Core()->m_Super || pChar->Core()->m_Invincible)
		return false;
	*pLocalId = LocalId;
	return true;
}

// ----------------------------------------------------------------------------
// Hazard grid
// ----------------------------------------------------------------------------

int CAvoidBlatant::ComputeTileMask(int Index) const
{
	const CCollision *pCollision = Collision();
	const int aTiles[3] = {
		pCollision->GetTileIndex(Index),
		pCollision->GetFrontTileIndex(Index),
		pCollision->SwitchLayer() ? pCollision->GetSwitchType(Index) : 0,
	};
	int Mask = 0;
	for(const int Tile : aTiles)
	{
		if(IsFreezeTile(Tile))
			Mask |= HZ_FREEZE;
		else if(Tile == TILE_DEATH)
			Mask |= HZ_DEATH;
		else if(IsTeleTile(Tile))
			Mask |= HZ_TELE;
	}
	if(aTiles[0] == TILE_SOLID || aTiles[0] == TILE_NOHOOK)
		Mask |= HZ_SOLID;
	return Mask;
}

// Classifies the map in small slices (about 2 ms per call) so loading a big map
// never causes a hitch. Until the grid is complete the missing part is computed
// directly from the collision data.
bool CAvoidBlatant::EnsureGrid()
{
	const CCollision *pCollision = Collision();
	const int Width = pCollision->GetWidth();
	const int Height = pCollision->GetHeight();
	const void *pKey = GameClient()->Layers() ? (const void *)GameClient()->Layers()->GameLayer() : (const void *)pCollision;
	if(pKey != m_pGridKey || Width != m_GridWidth || Height != m_GridHeight)
	{
		m_pGridKey = pKey;
		m_GridWidth = Width;
		m_GridHeight = Height;
		m_GridBuilt = 0;
		m_vHazard.assign((size_t)Width * (size_t)Height, 0);
	}
	const int Total = Width * Height;
	if(m_GridBuilt >= Total)
		return true;
	const int64_t Deadline = time_get() + time_freq() * 2 / 1000;
	int Index = m_GridBuilt;
	while(Index < Total)
	{
		const int ChunkEnd = std::min(Total, Index + 2048);
		for(; Index < ChunkEnd; ++Index)
			m_vHazard[Index] = (uint8_t)ComputeTileMask(Index);
		if(time_get() > Deadline)
			break;
	}
	m_GridBuilt = Index;
	return m_GridBuilt >= Total;
}

int CAvoidBlatant::ActiveMask() const
{
	int Mask = 0;
	if(g_Config.m_AvoidBlatantFreeze)
		Mask |= HZ_FREEZE;
	if(g_Config.m_AvoidBlatantDeath)
		Mask |= HZ_DEATH;
	if(g_Config.m_AvoidBlatantTele)
		Mask |= HZ_TELE;
	return Mask;
}

int CAvoidBlatant::TileMaskAt(int Nx, int Ny) const
{
	const int Index = Ny * m_GridWidth + Nx;
	if(Index < m_GridBuilt)
		return m_vHazard[Index];
	return ComputeTileMask(Index);
}

bool CAvoidBlatant::OutOfMap(vec2 Pos) const
{
	// The void behind the edge of the map is deadly, treat it like a death tile.
	if(!g_Config.m_AvoidBlatantDeath || m_GridWidth <= 0 || m_GridHeight <= 0)
		return false;
	return Pos.x - BODY_OFFSET < 0.0f || Pos.y - BODY_OFFSET < 0.0f ||
	       Pos.x + BODY_OFFSET >= m_GridWidth * TILE_SIZE || Pos.y + BODY_OFFSET >= m_GridHeight * TILE_SIZE;
}

// True if the axis aligned box around Pos overlaps any active hazard tile.
bool CAvoidBlatant::RectHazard(vec2 Pos, float Hx, float Hy) const
{
	const int Mask = ActiveMask();
	if(Mask == 0 || m_GridWidth <= 0 || m_GridHeight <= 0)
		return false;
	const int X0 = std::clamp((int)std::floor((Pos.x - Hx) / TILE_SIZE), 0, m_GridWidth - 1);
	const int X1 = std::clamp((int)std::floor((Pos.x + Hx) / TILE_SIZE), 0, m_GridWidth - 1);
	const int Y0 = std::clamp((int)std::floor((Pos.y - Hy) / TILE_SIZE), 0, m_GridHeight - 1);
	const int Y1 = std::clamp((int)std::floor((Pos.y + Hy) / TILE_SIZE), 0, m_GridHeight - 1);
	for(int Y = Y0; Y <= Y1; ++Y)
	{
		for(int X = X0; X <= X1; ++X)
		{
			if(TileMaskAt(X, Y) & Mask)
				return true;
		}
	}
	return false;
}

// Margin that may be kept on one axis along the given line of tiles. Where the
// free space between two obstacles (at least one of them a hazard) is narrower
// than the tee plus the wanted margin, the margin shrinks to what is physically
// available, so narrow passages stay passable. Horizontal: Line is a tile row and
// the scan runs along it; vertical: Line is a tile column and the scan runs down
// and up from the tee.
float CAvoidBlatant::PinchMargin(bool Horizontal, int Line, vec2 Pos, float Wanted) const
{
	if(Wanted <= 0.0f || !g_Config.m_AvoidBlatantNarrowPass || m_GridWidth <= 0 || m_GridHeight <= 0)
		return Wanted;
	const int Mask = ActiveMask();
	const int Tx = std::clamp((int)(Pos.x / TILE_SIZE), 0, m_GridWidth - 1);
	const int Ty = std::clamp((int)(Pos.y / TILE_SIZE), 0, m_GridHeight - 1);
	if(Horizontal ? (Line < 0 || Line >= m_GridHeight) : (Line < 0 || Line >= m_GridWidth))
		return Wanted;

	float aDist[2] = {-1.0f, -1.0f};
	bool aHazard[2] = {false, false};
	for(int Side = 0; Side < 2; ++Side)
	{
		const int Dir = Side == 0 ? -1 : 1;
		for(int Step = 0; Step <= SCAN_TILES; ++Step)
		{
			const int X = Horizontal ? Tx + Dir * Step : Line;
			const int Y = Horizontal ? Line : Ty + Dir * Step;
			bool Blocked;
			bool Hazard = false;
			const int Tile = Horizontal ? X : Y;
			if(X < 0 || Y < 0 || X >= m_GridWidth || Y >= m_GridHeight)
				Blocked = true; // the edge of the map blocks like a wall
			else
			{
				const int TileMask = TileMaskAt(X, Y);
				Hazard = (TileMask & Mask) != 0;
				Blocked = Hazard || (TileMask & HZ_SOLID) != 0;
			}
			if(!Blocked)
				continue;
			const float Boundary = (Dir > 0 ? Tile : Tile + 1) * TILE_SIZE;
			const float Coord = Horizontal ? Pos.x : Pos.y;
			aDist[Side] = std::max(0.0f, Dir > 0 ? Boundary - Coord : Coord - Boundary);
			aHazard[Side] = Hazard;
			break;
		}
	}
	if(aDist[0] < 0.0f || aDist[1] < 0.0f || !(aHazard[0] || aHazard[1]))
		return Wanted;
	const float Cap = (aDist[0] + aDist[1]) * 0.5f - BODY_OFFSET - NARROW_PLAY;
	return std::clamp(Cap, 0.0f, Wanted);
}

// Soft check: is the tee closer than the safe distance to a hazard tile? Every
// tile is judged on its own and gets its own relaxation, so the pinch ahead of
// the tee (for example the entrance of a one tile corridor) is recognised
// before the tee gets there.
bool CAvoidBlatant::SoftViolation(vec2 Pos) const
{
	const float Wanted = (float)std::clamp(g_Config.m_AvoidBlatantSafeDistance, 0, 64) + m_PingMargin;
	const int Mask = ActiveMask();
	if(Wanted <= 0.0f || Mask == 0 || m_GridWidth <= 0 || m_GridHeight <= 0)
		return false;
	const float Reach = BODY_OFFSET + Wanted;
	const int X0 = std::clamp((int)std::floor((Pos.x - Reach) / TILE_SIZE), 0, m_GridWidth - 1);
	const int X1 = std::clamp((int)std::floor((Pos.x + Reach) / TILE_SIZE), 0, m_GridWidth - 1);
	const int Y0 = std::clamp((int)std::floor((Pos.y - Reach) / TILE_SIZE), 0, m_GridHeight - 1);
	const int Y1 = std::clamp((int)std::floor((Pos.y + Reach) / TILE_SIZE), 0, m_GridHeight - 1);
	for(int Y = Y0; Y <= Y1; ++Y)
	{
		for(int X = X0; X <= X1; ++X)
		{
			if(!(TileMaskAt(X, Y) & Mask))
				continue;
			const float TileLeft = X * TILE_SIZE;
			const float TileTop = Y * TILE_SIZE;
			const float GapX = std::max({TileLeft - (Pos.x + BODY_OFFSET), (Pos.x - BODY_OFFSET) - (TileLeft + TILE_SIZE), 0.0f});
			const float GapY = std::max({TileTop - (Pos.y + BODY_OFFSET), (Pos.y - BODY_OFFSET) - (TileTop + TILE_SIZE), 0.0f});
			if(GapX >= Wanted || GapY >= Wanted)
				continue;
			const float Mx = PinchMargin(true, Y, Pos, Wanted);
			const float My = PinchMargin(false, X, Pos, Wanted);
			const bool InsideX = GapX == 0.0f || GapX < Mx;
			const bool InsideY = GapY == 0.0f || GapY < My;
			if(InsideX && InsideY)
				return true;
		}
	}
	return false;
}

// Hard check (real contact, exactly like the server) and, when enabled, the
// soft check with the safe distance.
bool CAvoidBlatant::BodyUnsafe(vec2 Pos) const
{
	if(OutOfMap(Pos))
		return true;
	if(RectHazard(Pos, BODY_OFFSET, BODY_OFFSET))
		return true;
	return m_SoftEnabled && SoftViolation(Pos);
}

// 0 = touching a hazard, higher = more room around the tee.
int CAvoidBlatant::ClearanceBucket(vec2 Pos) const
{
	for(int Step = 0; Step < NUM_CLEARANCE_STEPS; ++Step)
	{
		if(RectHazard(Pos, BODY_OFFSET + CLEARANCE_STEPS[Step], BODY_OFFSET + CLEARANCE_STEPS[Step]))
			return Step;
	}
	return MAX_CLEARANCE;
}

// ----------------------------------------------------------------------------
// Hook: along the cursor, opposite to it, or any direction that was verified
// to hit something hookable
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

void CAvoidBlatant::EvaluateHooks(const CNetObj_PlayerInput &Raw, int LocalId, SHookSet *pOut) const
{
	pOut->m_Num = 0;
	if(!g_Config.m_AvoidBlatantHookAssist)
		return;
	CCharacter *pChar = GameClient()->m_RegularPredictedWorld.GetCharacterById(LocalId);
	if(!pChar)
		return;
	vec2 Aim = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];
	if(length(Aim) < 1.0f)
		Aim = vec2((float)Raw.m_TargetX, (float)Raw.m_TargetY);
	if(length(Aim) < 1.0f)
		return;
	const vec2 Pos = pChar->GetPos();
	const vec2 Dir = normalize(Aim);

	auto Add = [&](int Mode, vec2 Direction) {
		if(pOut->m_Num >= MAX_HOOK_OPTIONS || !FindHookHit(Pos, Direction))
			return;
		SHookOption &Option = pOut->m_aOptions[pOut->m_Num++];
		Option.m_Mode = Mode;
		Option.m_X = round_to_int(Direction.x * 256.0f);
		Option.m_Y = round_to_int(Direction.y * 256.0f);
	};
	// Along the cursor first; if the cursor points into danger, the opposite side.
	Add(HOOK_AIM, Dir);
	Add(HOOK_MIRROR, -Dir);
	// Optional: any other direction. Only directions that hit a hookable tile are
	// offered, and the simulation decides whether one actually saves the tee.
	if(g_Config.m_AvoidBlatantHookFree)
	{
		for(int Index = 0; Index < 12; ++Index)
		{
			const float Angle = (float)Index * (2.0f * pi / 12.0f);
			const vec2 Direction(std::cos(Angle), std::sin(Angle));
			if(dot(Direction, Dir) > 0.94f || dot(Direction, -Dir) > 0.94f)
				continue;
			Add(HOOK_FREE, Direction);
		}
	}
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
	return !BodyUnsafe(*pCur);
}

CAvoidBlatant::SOutcome CAvoidBlatant::Simulate(int LocalId, const CNetObj_PlayerInput &First, int FirstTicks,
	const CNetObj_PlayerInput &Follow, int Ticks, vec2 *pPath, int *pNumPath, bool WantClearance) const
{
	SOutcome Result;
	Result.m_Survive = Ticks + 1;
	Result.m_MinClear = MAX_CLEARANCE;

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
		if(WantClearance)
			Result.m_MinClear = std::min(Result.m_MinClear, ClearanceBucket(Result.m_End));
		if(!Safe)
		{
			Result.m_Survive = Tick;
			break;
		}
	}
	// Survived the whole horizon, but ending in a fall towards a hazard is not safe.
	if(Result.m_Survive > Ticks && !TerminalSafe(SimWorld, LocalId))
		Result.m_Survive = Ticks;
	if(pNumPath)
		*pNumPath = PathCount;
	return Result;
}

// A plan is held for the whole remaining horizon, a hook included: hanging from
// a roof is a perfectly good way to stay safe. It is released automatically as
// soon as the user's own input is safe again, because every tick is decided anew.
bool CAvoidBlatant::PolicySurvives(CGameWorld &Start, int LocalId, const SCandidate &Policy, int Ticks) const
{
	CGameWorld Branch;
	Branch.CopyWorldClean(&Start);
	Branch.m_WorldConfig.m_PredictFreeze = 1;
	for(int Tick = 1; Tick <= Ticks; ++Tick)
	{
		vec2 Prev;
		vec2 Cur;
		if(!StepWorld(Branch, LocalId, Policy.m_Input, &Prev, &Cur))
			return false;
	}
	return TerminalSafe(Branch, LocalId);
}

// Robustness against latency: the plan is executed one tick late (the raw input
// is still sent on the first tick) and must still be safe.
bool CAvoidBlatant::DelayedSurvives(int LocalId, const CNetObj_PlayerInput &Raw, const SCandidate &Plan,
	int Horizon) const
{
	CGameWorld Branch;
	Branch.CopyWorldClean(&GameClient()->m_RegularPredictedWorld);
	Branch.m_WorldConfig.m_PredictFreeze = 1;
	vec2 Prev;
	vec2 Cur;
	if(!StepWorld(Branch, LocalId, Raw, &Prev, &Cur))
		return false;
	return PolicySurvives(Branch, LocalId, Plan, Horizon - 1);
}

// A cheap free flight estimate on the tile grid (gravity and air friction only,
// no hook, no air control). Returns the tick on which the tee would reach a
// hazard, or -1 if it lands on something solid first or nothing is hit.
int CAvoidBlatant::BallisticImpactTick(vec2 Pos, vec2 Vel) const
{
	if(m_GridWidth <= 0 || m_GridHeight <= 0)
		return -1;
	const CTuningParams &Tuning = GameClient()->m_aTuning[g_Config.m_ClDummy];
	const float Gravity = (float)Tuning.m_Gravity;
	const float AirFriction = (float)Tuning.m_AirFriction;
	for(int Tick = 1; Tick <= 62; ++Tick)
	{
		Vel.y += Gravity;
		Vel.x *= AirFriction;
		Pos += Vel;
		if(Pos.x < 0.0f || Pos.y < 0.0f || Pos.x >= m_GridWidth * TILE_SIZE || Pos.y >= m_GridHeight * TILE_SIZE)
			return Tick;
		const int Tx = (int)(Pos.x / TILE_SIZE);
		const int FeetRow = std::clamp((int)((Pos.y + BODY_OFFSET + 5.0f) / TILE_SIZE), 0, m_GridHeight - 1);
		const int HeadRow = std::clamp((int)((Pos.y - BODY_OFFSET - 5.0f) / TILE_SIZE), 0, m_GridHeight - 1);
		if(Vel.y > 0.0f && (TileMaskAt(Tx, FeetRow) & HZ_SOLID))
			return -1; // lands
		if(Vel.y < 0.0f && (TileMaskAt(Tx, HeadRow) & HZ_SOLID))
			Vel.y = 0.0f; // bumps its head
		if(RectHazard(Pos, BODY_OFFSET, BODY_OFFSET))
			return Tick;
	}
	return -1;
}

// A plan only counts as a rescue if the tee ends up in a stable state: hanging
// from a hook, or standing/landing on something solid, or flying on without ever
// reaching a hazard. A plan that merely postpones the fall into a hazard (a
// double jump over a freeze floor, for example) is not a rescue.
bool CAvoidBlatant::TerminalSafe(CGameWorld &World, int LocalId) const
{
	CCharacter *pChar = World.GetCharacterById(LocalId);
	if(!pChar)
		return false;
	if(pChar->Core()->m_HookState == HOOK_GRABBED)
		return true;
	return BallisticImpactTick(pChar->GetPos(), pChar->Core()->m_Vel) < 0;
}

// The horizon grows with ping (the predicted world is less exact), with the
// speed of the tee (a fast tee needs more ticks to stop) and, when the tee is in
// free flight towards a hazard, up to the moment of impact: a rescue such as a
// hook has to be started long before the fall reaches the hazard.
int CAvoidBlatant::ComputeHorizon(int LocalId, int PingTicks) const
{
	int Horizon = std::clamp(g_Config.m_AvoidBlatantPredictTicks + PingTicks / 2, 1, 62);
	if(CCharacter *pChar = GameClient()->m_RegularPredictedWorld.GetCharacterById(LocalId))
	{
		const vec2 Vel = pChar->Core()->m_Vel;
		Horizon = std::clamp(Horizon + (int)(length(Vel) * 0.8f), 1, 62);
		if(std::abs(Vel.y) > 0.5f || std::abs(Vel.x) > 0.5f)
		{
			const int Impact = BallisticImpactTick(pChar->GetPos(), Vel);
			if(Impact > 0)
				Horizon = std::clamp(std::max(Horizon, Impact + 8), 1, 62);
		}
	}
	return Horizon;
}

bool CAvoidBlatant::GreedyRollout(CGameWorld &Rolling, int LocalId, const CNetObj_PlayerInput &Raw,
	const SHookSet &Hooks, int Horizon, int64_t Deadline, CNetObj_PlayerInput *pFirst, int *pFirstHookMode,
	bool RequireStable) const
{
	struct SAction
	{
		int m_Direction;
		int m_Jump;
		int m_HookMode;
	};
	SAction aActions[12];
	int NumActions = 0;
	const bool CanChangeJump = (Raw.m_Jump & 1) != 0 || g_Config.m_AvoidBlatantAutoJump;
	const int aJumps[2] = {Raw.m_Jump, (Raw.m_Jump & 1) ? 0 : 1};
	const int NumJumps = CanChangeJump ? 2 : 1;
	int AimX = 0;
	int AimY = 0;
	bool HaveAim = false;
	for(int Index = 0; Index < Hooks.m_Num; ++Index)
	{
		if(Hooks.m_aOptions[Index].m_Mode == HOOK_AIM)
		{
			HaveAim = true;
			AimX = Hooks.m_aOptions[Index].m_X;
			AimY = Hooks.m_aOptions[Index].m_Y;
			break;
		}
	}
	for(int Direction = -1; Direction <= 1; ++Direction)
	{
		for(int JumpIndex = 0; JumpIndex < NumJumps; ++JumpIndex)
			aActions[NumActions++] = {Direction, aJumps[JumpIndex], HOOK_KEEP};
		if(!Raw.m_Hook && HaveAim)
			aActions[NumActions++] = {Direction, Raw.m_Jump, HOOK_AIM};
	}

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
				aActions[Index].m_HookMode, AimX, AimY, &Input);

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
			const int Score = ClearanceBucket(Cur) - CandidateCost(Raw, Input, aActions[Index].m_HookMode) * 2;
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
			aActions[BestIndex].m_HookMode, AimX, AimY, &Chosen);
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
	// As a viability test the rollout must also end in a stable state; as a last
	// resort rescue any delay is welcome.
	return !RequireStable || TerminalSafe(Rolling, LocalId);
}

// ----------------------------------------------------------------------------
// Candidates and decisions
// ----------------------------------------------------------------------------

void CAvoidBlatant::MakeInput(const CNetObj_PlayerInput &Raw, int Direction, int Jump, int HookMode,
	int TargetX, int TargetY, CNetObj_PlayerInput *pOut) const
{
	*pOut = Raw;
	pOut->m_Direction = Direction;
	pOut->m_Jump = Jump;
	if(HookMode == HOOK_RELEASE)
		pOut->m_Hook = 0;
	else if(HookMode == HOOK_AIM || HookMode == HOOK_MIRROR || HookMode == HOOK_FREE)
	{
		pOut->m_Hook = 1;
		pOut->m_TargetX = TargetX;
		pOut->m_TargetY = TargetY;
	}
}

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
	switch(HookMode)
	{
	case HOOK_RELEASE: Cost += 1; break;
	case HOOK_AIM: Cost += 3; break;
	case HOOK_MIRROR: Cost += 4; break;
	case HOOK_FREE: Cost += 5; break;
	default: break;
	}
	return Cost;
}

// Tier 0: direction and jump only. Tier 1: hook along (or against) the cursor.
// Tier 2: hook in any verified direction. Cheaper tiers are always tried first.
void CAvoidBlatant::BuildTiers(SContext &Context) const
{
	const CNetObj_PlayerInput &Raw = Context.m_Raw;
	// Releasing a held jump is always allowed; pressing a new one needs auto jump.
	const bool CanChangeJump = (Raw.m_Jump & 1) != 0 || g_Config.m_AvoidBlatantAutoJump;
	const int aJumps[2] = {Raw.m_Jump, (Raw.m_Jump & 1) ? 0 : 1};
	const int NumJumps = CanChangeJump ? 2 : 1;
	for(int Tier = 0; Tier < NUM_TIERS; ++Tier)
		Context.m_aNumTier[Tier] = 0;

	auto Add = [&](int Tier, int Direction, int Jump, int Mode, int TargetX, int TargetY) {
		if(Context.m_aNumTier[Tier] >= MAX_PER_TIER)
			return;
		if(Mode == HOOK_KEEP && Direction == Raw.m_Direction && Jump == Raw.m_Jump)
			return; // identical to the raw input
		SCandidate &Candidate = Context.m_aTier[Tier][Context.m_aNumTier[Tier]++];
		Candidate = SCandidate();
		MakeInput(Raw, Direction, Jump, Mode, TargetX, TargetY, &Candidate.m_Input);
		Candidate.m_HookMode = Mode;
		Candidate.m_TargetX = TargetX;
		Candidate.m_TargetY = TargetY;
		Candidate.m_Cost = CandidateCost(Raw, Candidate.m_Input, Mode);
	};

	for(int Direction = -1; Direction <= 1; ++Direction)
	{
		for(int JumpIndex = 0; JumpIndex < NumJumps; ++JumpIndex)
		{
			Add(0, Direction, aJumps[JumpIndex], HOOK_KEEP, 0, 0);
			if(Raw.m_Hook)
				Add(0, Direction, aJumps[JumpIndex], HOOK_RELEASE, 0, 0);
		}
	}
	{
		// Hook candidates are offered even while the user already holds the hook:
		// the direction is then overridden and the hook is simply kept.
		for(int Index = 0; Index < Context.m_Hooks.m_Num; ++Index)
		{
			const SHookOption &Option = Context.m_Hooks.m_aOptions[Index];
			if(Option.m_Mode == HOOK_FREE)
			{
				const int aDirections[3] = {0, Raw.m_Direction, -Raw.m_Direction};
				for(int DirIndex = 0; DirIndex < 3; ++DirIndex)
				{
					bool Duplicate = false;
					for(int Other = 0; Other < DirIndex; ++Other)
						Duplicate = Duplicate || aDirections[Other] == aDirections[DirIndex];
					if(!Duplicate)
						Add(2, aDirections[DirIndex], Raw.m_Jump, HOOK_FREE, Option.m_X, Option.m_Y);
				}
				continue;
			}
			for(int Direction = -1; Direction <= 1; ++Direction)
			{
				for(int JumpIndex = 0; JumpIndex < NumJumps; ++JumpIndex)
					Add(1, Direction, aJumps[JumpIndex], Option.m_Mode, Option.m_X, Option.m_Y);
			}
		}
	}

	// The escape plan that made the previous raw input viable stays available.
	if(m_HasHint && Context.m_Tick <= m_HintUntil)
	{
		const int Tier = m_HintHookMode == HOOK_FREE ? 2 : (m_HintHookMode == HOOK_AIM || m_HintHookMode == HOOK_MIRROR) ? 1 : 0;
		if(Context.m_aNumTier[Tier] < MAX_PER_TIER)
		{
			SCandidate &Candidate = Context.m_aTier[Tier][Context.m_aNumTier[Tier]++];
			Candidate = SCandidate();
			MakeInput(Raw, m_HintDirection, m_HintJump, m_HintHookMode, m_HintTargetX, m_HintTargetY, &Candidate.m_Input);
			Candidate.m_HookMode = m_HintHookMode;
			Candidate.m_TargetX = m_HintTargetX;
			Candidate.m_TargetY = m_HintTargetY;
			Candidate.m_Cost = CandidateCost(Raw, Candidate.m_Input, m_HintHookMode) - 1;
			Candidate.m_IsCommit = true;
		}
	}

	// The plan chosen on the previous tick competes with a small bonus so the
	// decision does not flip between equal alternatives.
	if(m_HasCommit && Context.m_Tick <= m_CommitUntil)
	{
		const int Tier = m_CommitHookMode == HOOK_FREE ? 2 : (m_CommitHookMode == HOOK_AIM || m_CommitHookMode == HOOK_MIRROR) ? 1 : 0;
		if(Context.m_aNumTier[Tier] < MAX_PER_TIER)
		{
			SCandidate &Candidate = Context.m_aTier[Tier][Context.m_aNumTier[Tier]++];
			Candidate = SCandidate();
			MakeInput(Raw, m_CommitDirection, m_CommitJump, m_CommitHookMode, m_CommitTargetX, m_CommitTargetY,
				&Candidate.m_Input);
			Candidate.m_HookMode = m_CommitHookMode;
			Candidate.m_TargetX = m_CommitTargetX;
			Candidate.m_TargetY = m_CommitTargetY;
			Candidate.m_Cost = CandidateCost(Raw, Candidate.m_Input, m_CommitHookMode) - 1;
			Candidate.m_IsCommit = true;
		}
	}
}

// Viability check. The raw input is applied for exactly one tick. The resulting
// state is "viable" if at least one escape plan keeps the tee safe for the rest
// of the horizon. Plans are tried gentlest first and the first one that works
// ends the search, so in open space this costs a single short simulation.
bool CAvoidBlatant::RawIsViable(SContext &Context, SCandidate *pWinner) const
{
	CGameWorld Next;
	Next.CopyWorldClean(&GameClient()->m_RegularPredictedWorld);
	Next.m_WorldConfig.m_PredictFreeze = 1;
	vec2 Prev;
	vec2 Cur;
	if(!StepWorld(Next, Context.m_LocalId, Context.m_Raw, &Prev, &Cur))
		return false; // the raw input itself walks into danger on this very tick
	const int Remaining = Context.m_Horizon - 1;
	if(Remaining <= 0)
		return true;

	for(int Tier = 0; Tier < NUM_TIERS; ++Tier)
	{
		const int Count = Context.m_aNumTier[Tier];
		int aOrder[MAX_PER_TIER];
		int aKey[MAX_PER_TIER];
		for(int Index = 0; Index < Count; ++Index)
		{
			const SCandidate &Policy = Context.m_aTier[Tier][Index];
			int Key = 0;
			if(Policy.m_Input.m_Direction != 0)
				Key += 2;
			if((Policy.m_Input.m_Jump & 1) != (Context.m_Raw.m_Jump & 1))
				Key += 3;
			aOrder[Index] = Index;
			aKey[Index] = Key;
		}
		// Stopping first, then steering, then jumping.
		std::stable_sort(aOrder, aOrder + Count, [&](int A, int B) { return aKey[A] < aKey[B]; });
		for(int Rank = 0; Rank < Count; ++Rank)
		{
			if(time_get() > Context.m_ViabilityDeadline)
				return false;
			if(PolicySurvives(Next, Context.m_LocalId, Context.m_aTier[Tier][aOrder[Rank]], Remaining))
			{
				if(pWinner)
					*pWinner = Context.m_aTier[Tier][aOrder[Rank]];
				return true;
			}
		}
	}

	// Last tier: a greedy rollout can find plans that change action mid-way.
	if(time_get() < Context.m_ViabilityDeadline)
	{
		CGameWorld Rolling;
		Rolling.CopyWorldClean(&Next);
		Rolling.m_WorldConfig.m_PredictFreeze = 1;
		CNetObj_PlayerInput First = Context.m_Raw;
		int FirstHookMode = HOOK_KEEP;
		return GreedyRollout(Rolling, Context.m_LocalId, Context.m_Raw, Context.m_Hooks, std::min(Remaining, 12),
			Context.m_ViabilityDeadline, &First, &FirstHookMode, true);
	}
	return false;
}

CAvoidBlatant::SDecision CAvoidBlatant::Decide(SContext &Context)
{
	SDecision Result;
	Result.m_Plan = Context.m_Raw;
	Result.m_State = STATE_SAFE;
	const int Horizon = Context.m_Horizon;
	// A rescue must keep the tee safe a bit longer than the warning horizon, so a
	// plan that only postpones the impact does not count as a rescue.
	const int PlanHorizon = std::min(62, Horizon + 12);

	m_NumBasePath = 0;
	m_NumPlanPath = 0;
	const SOutcome Base = Simulate(Context.m_LocalId, Context.m_Raw, Horizon, Context.m_Raw, Horizon,
		m_aBasePath, &m_NumBasePath, false);
	m_BaseHazardIndex = Base.m_Survive <= Horizon ? Base.m_Survive : -1;
	m_DebugBaseSurvive = Base.m_Survive;
	Result.m_Survive = Base.m_Survive;
	if(Base.m_Survive > Horizon)
	{
		// Holding the raw input is safe for the whole horizon: do not interfere.
		Result.m_Resolved = true;
		return Result;
	}

	if(Context.m_Hooks.m_Num == 0 && Context.m_aNumTier[0] == 0)
	{
		EvaluateHooks(Context.m_Raw, Context.m_LocalId, &Context.m_Hooks);
		BuildTiers(Context);
	}
	m_DebugCandidates = Context.m_aNumTier[0] + Context.m_aNumTier[1] + Context.m_aNumTier[2];

	// 1. Viability: the raw input may be used if, after this tick, an escape plan
	// still exists. This lets the tee get close to a danger without ever taking
	// the step that has no way out.
	SCandidate Winner;
	Winner.m_HookMode = -1;
	if(RawIsViable(Context, &Winner))
	{
		if(Winner.m_HookMode >= 0)
		{
			m_HasHint = true;
			m_HintUntil = Context.m_Tick + 3;
			m_HintDirection = Winner.m_Input.m_Direction;
			m_HintJump = Winner.m_Input.m_Jump;
			m_HintHookMode = Winner.m_HookMode;
			m_HintTargetX = Winner.m_TargetX;
			m_HintTargetY = Winner.m_TargetY;
		}
		Result.m_State = STATE_VIABLE;
		Result.m_Resolved = true;
		return Result;
	}

	// 2. The raw input leads into a dead end: look for the best safe plan, tier by
	// tier. Among the safe plans of a tier the one with the largest clearance that
	// also survives a one tick delay wins; the user's input is changed as little
	// as possible.
	int PartialTier = -1;
	int PartialIndex = -1;
	int PartialSurvive = Base.m_Survive;
	for(int Tier = 0; Tier < NUM_TIERS; ++Tier)
	{
		const int Count = Context.m_aNumTier[Tier];
		if(Count == 0)
			continue;
		if(Tier > 0 && time_get() > Context.m_HardDeadline)
			break;
		int aOrder[MAX_PER_TIER];
		for(int Index = 0; Index < Count; ++Index)
			aOrder[Index] = Index;
		std::stable_sort(aOrder, aOrder + Count, [&](int A, int B) {
			return Context.m_aTier[Tier][A].m_Cost < Context.m_aTier[Tier][B].m_Cost;
		});

		int aFull[MAX_FULL_SAFE];
		int NumFull = 0;
		for(int Rank = 0; Rank < Count; ++Rank)
		{
			SCandidate &Candidate = Context.m_aTier[Tier][aOrder[Rank]];
			Candidate.m_Outcome = Simulate(Context.m_LocalId, Candidate.m_Input, PlanHorizon, Candidate.m_Input, PlanHorizon,
				nullptr, nullptr, true);
			if(Candidate.m_Outcome.m_Survive > PlanHorizon)
			{
				aFull[NumFull++] = aOrder[Rank];
				if(NumFull >= MAX_FULL_SAFE)
					break;
			}
			else if(Candidate.m_Outcome.m_Survive > PartialSurvive)
			{
				PartialSurvive = Candidate.m_Outcome.m_Survive;
				PartialTier = Tier;
				PartialIndex = aOrder[Rank];
			}
			if(time_get() > Context.m_HardDeadline)
				break;
		}

		if(NumFull > 0)
		{
			// Rank the safe plans cheaply (clearance, then least change), then test
			// the two best against disturbances and pick the most dependable one.
			int aRank[MAX_FULL_SAFE];
			for(int Slot = 0; Slot < NumFull; ++Slot)
				aRank[Slot] = Slot;
			auto PreScore = [&](int Slot) {
				const SCandidate &Candidate = Context.m_aTier[Tier][aFull[Slot]];
				// Among safe plans prefer the one that keeps the tee moving the way the
				// user wants (for example jumping over a pit instead of stopping).
				const float Progress = (Candidate.m_Outcome.m_End.x - Context.m_StartPos.x) * (float)Context.m_Raw.m_Direction;
				const int ProgressBonus = std::clamp((int)(Progress / 48.0f), -2, 3);
				return Candidate.m_Outcome.m_MinClear - Candidate.m_Cost * 2 + ProgressBonus;
			};
			std::stable_sort(aRank, aRank + NumFull, [&](int A, int B) { return PreScore(A) > PreScore(B); });
			const int NumTested = NumFull > 1 ? std::min(NumFull, 2) : 0;
			int BestSlot = aRank[0];
			int BestScore = INT_MIN;
			for(int Rank = 0; Rank < NumFull; ++Rank)
			{
				const int Slot = aRank[Rank];
				SCandidate &Candidate = Context.m_aTier[Tier][aFull[Slot]];
				Candidate.m_Robust = 5;
				// Latency robustness: the plan must survive being applied one tick late.
				if(Rank < NumTested && time_get() < Context.m_HardDeadline)
					Candidate.m_Robust = DelayedSurvives(Context.m_LocalId, Context.m_Raw, Candidate, PlanHorizon) ? 5 : 0;
				else if(Rank >= NumTested && NumTested > 0)
					Candidate.m_Robust = 0; // not tested: do not prefer it over a tested plan
				const int Score = Candidate.m_Robust * 10 + PreScore(Slot);
				if(Score > BestScore)
				{
					BestScore = Score;
					BestSlot = Slot;
				}
			}
			const SCandidate &Best = Context.m_aTier[Tier][aFull[BestSlot]];
			Result.m_Override = true;
			Result.m_Resolved = true;
			Result.m_Plan = Best.m_Input;
			Result.m_HookMode = Best.m_HookMode;
			Result.m_TargetX = Best.m_TargetX;
			Result.m_TargetY = Best.m_TargetY;
			Result.m_Survive = Best.m_Outcome.m_Survive;
			Result.m_State = Best.m_IsCommit ? STATE_COMMIT : STATE_BEST_PLAN;
			m_DebugTier = Tier;
			m_DebugClear = Best.m_Outcome.m_MinClear;
			return Result;
		}
	}

	// 3. Nothing saves the tee outright: greedy rollout, which picks the best
	// single action on every tick of the horizon.
	if(Context.m_Tick >= m_GreedyCooldownUntil)
	{
		const int64_t GreedyDeadline = time_get() + time_freq() * 3 / 1000;
		CGameWorld Rolling;
		Rolling.CopyWorldClean(&GameClient()->m_RegularPredictedWorld);
		Rolling.m_WorldConfig.m_PredictFreeze = 1;
		CNetObj_PlayerInput First = Context.m_Raw;
		int FirstHookMode = HOOK_KEEP;
		if(GreedyRollout(Rolling, Context.m_LocalId, Context.m_Raw, Context.m_Hooks, std::min(Horizon, 12),
			   GreedyDeadline, &First, &FirstHookMode, false))
		{
			Result.m_Override = true;
			Result.m_Resolved = true;
			Result.m_Plan = First;
			Result.m_HookMode = FirstHookMode;
			Result.m_TargetX = First.m_TargetX;
			Result.m_TargetY = First.m_TargetY;
			Result.m_Survive = Horizon + 1;
			Result.m_State = STATE_GREEDY;
			return Result;
		}
		m_GreedyCooldownUntil = Context.m_Tick + 3;
	}

	// 4. Still take the plan that delays the hazard the most, if there is one.
	if(PartialTier >= 0)
	{
		const SCandidate &Best = Context.m_aTier[PartialTier][PartialIndex];
		Result.m_Override = true;
		Result.m_Plan = Best.m_Input;
		Result.m_HookMode = Best.m_HookMode;
		Result.m_TargetX = Best.m_TargetX;
		Result.m_TargetY = Best.m_TargetY;
		Result.m_Survive = Best.m_Outcome.m_Survive;
		Result.m_State = STATE_BEST_PLAN;
		return Result;
	}
	Result.m_State = STATE_NO_SOLUTION;
	return Result;
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
	EnsureGrid();

	SContext Context;
	Context.m_LocalId = LocalId;
	Context.m_Tick = Tick;
	Context.m_Raw = *pInput;
	if(CCharacter *pStartChar = GameClient()->m_RegularPredictedWorld.GetCharacterById(LocalId))
		Context.m_StartPos = pStartChar->GetPos();
	// Time budgets: the viability search may use 3 ms, the whole decision 8 ms.
	Context.m_ViabilityDeadline = StartTime + time_freq() * 3 / 1000;
	Context.m_HardDeadline = StartTime + time_freq() * 8 / 1000;

	// Ping compensation. With a high ping the predicted world is less exact, so
	// the body gets a small safety margin (below 3 px, so a one tile gap still
	// fits) and the horizon is a little longer.
	int PingMs = 0;
	if(const CNetObj_PlayerInfo *pInfo = GameClient()->m_Snap.m_apPlayerInfos[LocalId])
		PingMs = std::clamp(pInfo->m_Latency, 0, 400);
	const int PingTicks = std::clamp(PingMs / 20, 0, 12);
	m_PingMargin = std::min(PingTicks * 0.25f, 3.0f);
	Context.m_Horizon = ComputeHorizon(LocalId, PingTicks);
	Context.m_Commit = std::clamp(g_Config.m_AvoidBlatantCommitTicks, 1, 20);
	m_DebugPingTicks = PingTicks;
	m_DebugHorizon = Context.m_Horizon;
	m_VisualTick = Tick;
	m_DebugTier = 0;
	m_DebugClear = 0;
	m_DebugHookMode = HOOK_KEEP;

	// The safe distance is only enforced while the tee is outside of that zone;
	// a tee that already stands inside it is only held back from real contact.
	bool SoftWanted = false;
	if((float)g_Config.m_AvoidBlatantSafeDistance + m_PingMargin > 0.0f)
	{
		CCharacter *pChar = GameClient()->m_RegularPredictedWorld.GetCharacterById(LocalId);
		m_SoftEnabled = true;
		SoftWanted = pChar && !BodyUnsafe(pChar->GetPos());
	}

	SDecision Decision;
	for(int Pass = 0; Pass < 2; ++Pass)
	{
		if(Pass == 0 && !SoftWanted)
			continue;
		// Pass 0 keeps the safe distance, pass 1 (only if pass 0 found nothing)
		// settles for avoiding real contact.
		m_SoftEnabled = Pass == 0;
		m_DebugSoft = m_SoftEnabled;
		Decision = Decide(Context);
		if(Decision.m_Resolved)
			break;
	}

	if(Decision.m_Override)
	{
		m_HasCommit = true;
		m_CommitUntil = Tick + Context.m_Commit;
		m_CommitDirection = Decision.m_Plan.m_Direction;
		m_CommitJump = Decision.m_Plan.m_Jump;
		m_CommitHookMode = Decision.m_HookMode;
		m_CommitTargetX = Decision.m_TargetX;
		m_CommitTargetY = Decision.m_TargetY;
		if(g_Config.m_AvoidBlatantVisualPath)
		{
			const int PathHorizon = std::min(62, Context.m_Horizon + 12);
			Simulate(LocalId, Decision.m_Plan, PathHorizon, Decision.m_Plan, PathHorizon, m_aPlanPath,
				&m_NumPlanPath, false);
		}
	}
	else
		ClearCommit();

	m_State = Decision.m_State;
	m_DebugPlanSurvive = Decision.m_Survive;
	m_DebugHookMode = Decision.m_HookMode;
	if(Decision.m_Override)
	{
		ApplyPlan(pInput, Decision.m_Plan);
		m_Overriding = true;
	}
	const int64_t Elapsed = time_get() - StartTime;
	m_DebugMicros = (int)(Elapsed * 1000000 / std::max<int64_t>(time_freq(), 1));

	m_CachedTick = Tick;
	m_CachedRaw = Context.m_Raw;
	m_CachedOverride = Decision.m_Override;
	m_CachedOut = *pInput;
	return Decision.m_Override;
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
		char aBuf[240];
		str_format(aBuf, sizeof(aBuf),
			"Avoid: %s | base %d | plan %d | hor %d | ping %d | tier %d | clr %d | %s | %s | %d us",
			StateName(m_State), m_DebugBaseSurvive, m_DebugPlanSurvive, m_DebugHorizon,
			m_DebugPingTicks, m_DebugTier, m_DebugClear, m_DebugSoft ? "soft" : "hard",
			HookName(m_DebugHookMode), m_DebugMicros);
		const vec2 Origin = m_aBasePath[0];
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.95f);
		TextRender()->Text(Origin.x - 110.0f, Origin.y - 80.0f, 14.0f, aBuf);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	}
}
