#include "zz_hook_aimbot.h"

#include <algorithm>
#include <cmath>

#include <base/math.h>

#include <engine/friends.h>
#include <engine/shared/config.h>

#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/mapitems.h>

namespace
{
constexpr float TARGET_LOCK_TOLERANCE = 0.20f;
constexpr float MIN_AIM_DISTANCE = 0.001f;
constexpr int HOOK_ANGLE_SAMPLES = 12;
constexpr float HIT_MARGIN_TOLERANCE = 0.25f;

float AimAngle(const vec2 &From, const vec2 &To)
{
	return std::acos(std::clamp(dot(From, To), -1.0f, 1.0f));
}

vec2 Rotate(const vec2 &Direction, float Angle)
{
	const float Sin = std::sin(Angle);
	const float Cos = std::cos(Angle);
	return vec2(Direction.x * Cos - Direction.y * Sin, Direction.x * Sin + Direction.y * Cos);
}
}

void CZZHookAimbot::ClearLock(int DummyIndex)
{
	if(DummyIndex >= 0 && DummyIndex < NUM_DUMMIES)
		m_aLockedTarget[DummyIndex] = -1;
}

void CZZHookAimbot::OnReset()
{
	for(int &LockedTarget : m_aLockedTarget)
		LockedTarget = -1;
}

void CZZHookAimbot::OnStateChange(int NewState, int OldState)
{
	OnReset();
}

bool CZZHookAimbot::IsFrozen(int ClientId, int DummyIndex) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return false;

	CCharacter *pCharacter = GameClient()->m_RegularPredictedWorld.GetCharacterById(ClientId);
	if(pCharacter && (pCharacter->Core()->m_IsInFreeze || pCharacter->m_FreezeTime > 0))
		return true;

	const CGameClient::CClientData &ClientData = GameClient()->m_aClients[ClientId];
	return ClientData.m_DeepFrozen || ClientData.m_LiveFrozen ||
		ClientData.m_FreezeEnd > Client()->GameTick(DummyIndex) ||
		ClientData.m_Predicted.m_IsInFreeze != 0;
}

bool CZZHookAimbot::IsIgnoredFriend(int ClientId) const
{
	if(!g_Config.m_ClZzAimbotIgnoreFriends || ClientId < 0 || ClientId >= MAX_CLIENTS)
		return false;

	const CGameClient::CClientData &Client = GameClient()->m_aClients[ClientId];
	// m_Friend is snapshot-cached and can lag one snapshot behind changes made in
	// the player list. Query the authoritative friend list as well so the filter
	// also works immediately after toggling a friend or enabling the option.
	const CWarDataCache &WarData = GameClient()->m_WarList.GetWarData(ClientId);
	const bool TClientTeammate = WarData.m_WarGroupMatches.size() > 2 &&
		WarData.m_WarGroupMatches[2];
	return Client.m_Friend || TClientTeammate ||
		(GameClient()->Friends() &&
			GameClient()->Friends()->IsFriend(Client.m_aName, Client.m_aClan, true));
}

bool CZZHookAimbot::BuildTarget(int ClientId, int DummyIndex,
	const vec2 &LocalPos, const vec2 &UserAim, float HalfFov, float HookLength,
	const SHookPrediction &Prediction, STarget &Target) const
{
	CCharacter *pLocalCharacter = GameClient()->m_RegularPredictedWorld.GetCharacterById(
		GameClient()->m_Snap.m_LocalClientId);
	if(!pLocalCharacter || pLocalCharacter->Core()->m_HookHitDisabled ||
		!pLocalCharacter->CanCollide(ClientId) ||
		GameClient()->m_aTuning[DummyIndex].m_PlayerHooking <= 0)
		return false;

	const vec2 CurrentPos = Prediction.m_aaPos[ClientId][0];
	const float CenterDistance = distance(LocalPos, CurrentPos);
	if(CenterDistance < MIN_AIM_DISTANCE)
		return false;

	const float HookSpeed = std::max(1.0f, (float)GameClient()->m_aTuning[DummyIndex].m_HookFireSpeed);
	const float TeeRadius = CCharacterCore::PhysicalSize();
	const float HookHitRadius = TeeRadius + 2.0f;
	const float MaxAimDistance = HookLength + HookHitRadius;

	vec2 BestDirection = vec2(1.0f, 0.0f);
	float BestPointAngle = 1e18f;
	float BestPointDistance = 1e18f;
	float BestHitMargin = -1.0f;
	int BestHitTick = Prediction.m_Ticks;
	bool Found = false;

	const auto TryDirection = [&](const vec2 &Direction) {
		const float Angle = AimAngle(UserAim, Direction);
		if(Angle > HalfFov)
			return;

		// Match CCharacterCore::Tick exactly enough for aiming: the hook starts in
		// front of the tee and advances in HookFireSpeed-sized segments. Players
		// are tested on every segment before the ground result is applied.
		vec2 HookPos = LocalPos + Direction * TeeRadius * 1.5f;
		if(HookLength <= distance(LocalPos, HookPos))
			return;

		for(int Step = 0; Step < Prediction.m_Ticks; ++Step)
		{
			const vec2 HookBase = Prediction.m_aaPos[GameClient()->m_Snap.m_LocalClientId][Step];
			vec2 SegmentEnd = HookPos + Direction * HookSpeed;
			bool ReachedMaxLength = false;
			if(distance(HookBase, SegmentEnd) > HookLength)
			{
				SegmentEnd = HookBase + normalize(SegmentEnd - HookBase) * HookLength;
				ReachedMaxLength = true;
			}

			vec2 CollisionPos = SegmentEnd;
			int TeleNr = 0;
			const int Hit = Collision()->IntersectLineTeleHook(
				HookPos, SegmentEnd, &CollisionPos, nullptr, &TeleNr);
			const vec2 ActualEnd = Hit != 0 ? CollisionPos : SegmentEnd;

			if(!Prediction.m_aaActive[ClientId][Step])
				break;
			const vec2 TargetPos = Prediction.m_aaPos[ClientId][Step];
			vec2 Closest;
			if(closest_point_on_line(HookPos, ActualEnd, TargetPos, Closest) &&
				distance(TargetPos, Closest) < HookHitRadius)
			{
				const float MissDistance = distance(TargetPos, Closest);
				const float TargetAlong = distance(HookPos, Closest);
				bool OtherPlayerFirst = false;
				for(int OtherId = 0; OtherId < MAX_CLIENTS; ++OtherId)
				{
					if(OtherId == ClientId ||
						OtherId == GameClient()->m_Snap.m_LocalClientId ||
						!Prediction.m_aaActive[OtherId][Step] ||
						!pLocalCharacter->CanCollide(OtherId))
						continue;
					vec2 OtherClosest;
					const vec2 OtherPos = Prediction.m_aaPos[OtherId][Step];
					if(closest_point_on_line(HookPos, ActualEnd, OtherPos,
						   OtherClosest) &&
						distance(OtherPos, OtherClosest) < HookHitRadius &&
						distance(HookPos, OtherClosest) < TargetAlong)
					{
						OtherPlayerFirst = true;
						break;
					}
				}
				if(!OtherPlayerFirst)
				{
					const float TargetDistance = distance(LocalPos, TargetPos);
					const float HitMargin = HookHitRadius - MissDistance;
					// Prefer a deep, stable intersection through the tee over the ray
					// closest to the user's cursor. The previous ordering deliberately
					// selected grazing edge hits, which are very sensitive to one tick
					// of network/prediction error while either tee is moving.
					if(HitMargin > BestHitMargin + HIT_MARGIN_TOLERANCE ||
						(std::abs(HitMargin - BestHitMargin) <= HIT_MARGIN_TOLERANCE &&
							(Step < BestHitTick ||
								(Step == BestHitTick &&
									(Angle < BestPointAngle ||
										(std::abs(Angle - BestPointAngle) < 0.0005f &&
											TargetDistance < BestPointDistance))))))
					{
						BestDirection = Direction;
						BestPointAngle = Angle;
						BestPointDistance = TargetDistance;
						BestHitMargin = HitMargin;
						BestHitTick = Step;
						Found = true;
					}
					return;
				}
			}

			// Tele-hooks are intentionally not guessed: their exit is server-random
			// when multiple outputs exist. A regular solid/nohook collision ends this
			// candidate exactly as it ends the outgoing hook segment.
			if(Hit != 0 || ReachedMaxLength)
				break;
			HookPos = SegmentEnd;
		}
	};

	const auto ScanTargetAngles = [&](const vec2 &TargetPos) {
		const vec2 Delta = TargetPos - LocalPos;
		const float Distance = length(Delta);
		if(Distance < MIN_AIM_DISTANCE || Distance > MaxAimDistance)
			return;

		const vec2 CenterDirection = Delta * (1.0f / Distance);
		const float EdgeAngle = std::asin(std::clamp(HookHitRadius / Distance, 0.0f, 1.0f));
		for(int Sample = -HOOK_ANGLE_SAMPLES; Sample <= HOOK_ANGLE_SAMPLES; ++Sample)
		{
			const float Offset = EdgeAngle * (float)Sample / (float)HOOK_ANGLE_SAMPLES;
			TryDirection(Rotate(CenterDirection, Offset));
		}
	};

	// Aim at every predicted position that the real hook can reach. This handles
	// walking, falling, acceleration and tile collisions instead of applying one
	// fixed velocity lead to a stationary target.
	for(int Step = 0; Step < Prediction.m_Ticks; ++Step)
	{
		if(!Prediction.m_aaActive[ClientId][Step])
			break;
		ScanTargetAngles(Prediction.m_aaPos[ClientId][Step]);
	}

	if(!Found)
		return false;

	vec2 CenterDirection = CurrentPos - LocalPos;
	CenterDirection = normalize(CenterDirection);
	const float FovScale = std::max(HalfFov, 0.01f);
	const float CenterAngle = AimAngle(UserAim, CenterDirection);
	Target.m_ClientId = ClientId;
	Target.m_AimDirection = BestDirection;
	Target.m_Frozen = IsFrozen(ClientId, DummyIndex);
	const float StabilityPenalty = 1.0f -
		std::clamp(BestHitMargin / HookHitRadius, 0.0f, 1.0f);
	Target.m_Score = CenterAngle / FovScale * 0.62f +
		BestPointAngle / FovScale * 0.20f +
		BestPointDistance / std::max(HookLength, 1.0f) * 0.05f +
		StabilityPenalty * 0.13f;
	return true;
}

bool CZZHookAimbot::Apply(CNetObj_PlayerInput *pInput, int DummyIndex,
	const vec2 &LocalPos, const vec2 &UserAim, bool InputBlocked, bool TeeOnly)
{
	DummyIndex = std::clamp(DummyIndex, 0, NUM_DUMMIES - 1);
	if(InputBlocked ||
		(!TeeOnly && !g_Config.m_ClZzAimbotEnabled && !g_Config.m_ClKnHookBlocks))
	{
		ClearLock(DummyIndex);
		return false;
	}
	// Keep the acquired tee across a release/re-hook packet. Clearing the lock on
	// every even hook tick was the main source of target flicker.
	if(!pInput->m_Hook)
		return false;

	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	if(LocalId < 0 || LocalId >= MAX_CLIENTS)
	{
		ClearLock(DummyIndex);
		return false;
	}

	vec2 AimDirection = UserAim;
	if(length(AimDirection) < MIN_AIM_DISTANCE)
		AimDirection = vec2(1.0f, 0.0f);
	else
		AimDirection = normalize(AimDirection);
	const float HalfFov = std::clamp((float)g_Config.m_ClZzAimbotFov, 1.0f, 180.0f) * (pi / 360.0f);
	const float HookLength = (float)GameClient()->m_aTuning[DummyIndex].m_HookLength;
	const float HookSpeed = std::max(1.0f,
		(float)GameClient()->m_aTuning[DummyIndex].m_HookFireSpeed);
	SHookPrediction Prediction;
	Prediction.m_Ticks = std::clamp(
		(int)std::ceil(std::max(0.0f,
			HookLength - CCharacterCore::PhysicalSize() * 1.5f) /
			HookSpeed) + 2,
		2, MAX_HOOK_PREDICTION_TICKS);

	// One shared world copy predicts all character positions. BuildTarget then
	// tests the outgoing hook against those per-tick positions, so the cost does
	// not grow by copying the world for every aim ray or every client.
	CGameWorld PredictionWorld;
	PredictionWorld.CopyWorldClean(&GameClient()->m_RegularPredictedWorld);
	CNetObj_PlayerInput MovementInput = *pInput;
	MovementInput.m_Hook = 0;
	MovementInput.m_Fire = (MovementInput.m_Fire & 1) ?
		(MovementInput.m_Fire + 1) & INPUT_STATE_MASK : MovementInput.m_Fire;
	MovementInput.m_WantedWeapon = 0;
	MovementInput.m_NextWeapon = 0;
	MovementInput.m_PrevWeapon = 0;
	for(int Step = 0; Step < Prediction.m_Ticks; ++Step)
	{
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			CCharacter *pCharacter = PredictionWorld.GetCharacterById(ClientId);
			if(!pCharacter)
				continue;
			Prediction.m_aaPos[ClientId][Step] = pCharacter->Core()->m_Pos;
			Prediction.m_aaActive[ClientId][Step] = true;
		}
		if(Step + 1 >= Prediction.m_Ticks)
			break;
		if(CCharacter *pSimLocal = PredictionWorld.GetCharacterById(LocalId))
			pSimLocal->OnPredictedInput(&MovementInput);
		PredictionWorld.m_GameTick++;
		PredictionWorld.Tick();
	}
	STarget BestTarget;
	STarget BestFrozenTarget;
	STarget LockedTarget;

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(ClientId == LocalId || !GameClient()->m_Snap.m_aCharacters[ClientId].m_Active)
			continue;
		if(IsIgnoredFriend(ClientId))
			continue;

		STarget Candidate;
		if(!BuildTarget(ClientId, DummyIndex, LocalPos, AimDirection, HalfFov,
			HookLength, Prediction, Candidate))
			continue;
		if(!BestTarget.Valid() || Candidate.m_Score < BestTarget.m_Score)
			BestTarget = Candidate;
		if(Candidate.m_Frozen && (!BestFrozenTarget.Valid() || Candidate.m_Score < BestFrozenTarget.m_Score))
			BestFrozenTarget = Candidate;
		if(ClientId == m_aLockedTarget[DummyIndex])
			LockedTarget = Candidate;
	}
	if(m_aLockedTarget[DummyIndex] >= 0 && !LockedTarget.Valid())
	{
		const int LockedId = m_aLockedTarget[DummyIndex];
		if(LockedId != LocalId && LockedId < MAX_CLIENTS &&
			GameClient()->m_Snap.m_aCharacters[LockedId].m_Active &&
			!IsIgnoredFriend(LockedId))
			BuildTarget(LockedId, DummyIndex, LocalPos, AimDirection,
				minimum(HalfFov * 1.20f, pi), HookLength, Prediction, LockedTarget);
	}

	STarget Selected = g_Config.m_ClZzAimbotPreferFrozen && BestFrozenTarget.Valid() ? BestFrozenTarget : BestTarget;
	if(!Selected.Valid() && LockedTarget.Valid())
		Selected = LockedTarget;
	if(!Selected.Valid())
	{
		ClearLock(DummyIndex);
		// Hook Drive owns the wall fallback while it is enabled. Tee selection
		// above still has strict priority, but returning here lets Hook Drive
		// perform its rapid release/re-hook cycle on the nearest block.
		if(TeeOnly || !g_Config.m_ClKnHookBlocks || g_Config.m_ClZzFlyRide)
			return false;

		// Players always win. Only when no tee can be hooked do we silently snap
		// the packet aim to the nearest hookable wall inside the configured FOV.
		vec2 BestDirection;
		float BestAngle = 1e18f;
		constexpr int WALL_SAMPLES = 96;
		for(int Sample = -WALL_SAMPLES; Sample <= WALL_SAMPLES; ++Sample)
		{
			const float Offset = HalfFov * (float)Sample / (float)WALL_SAMPLES;
			const vec2 Direction = Rotate(AimDirection, Offset);
			const vec2 Start = LocalPos + Direction *
				CCharacterCore::PhysicalSize() * 1.5f;
			const vec2 End = LocalPos + Direction * HookLength;
			vec2 HitPos;
			int TeleNr = 0;
			const int Hit = Collision()->IntersectLineTeleHook(Start, End, &HitPos, nullptr, &TeleNr);
			if(Hit == 0 || Hit == TILE_NOHOOK || Hit == TILE_TELEINHOOK ||
				TeleNr != 0)
				continue;
			bool HitsIgnoredFriend = false;
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
			{
				if(!Prediction.m_aaActive[ClientId][0] || !IsIgnoredFriend(ClientId))
					continue;
				vec2 Closest;
				if(closest_point_on_line(Start, HitPos,
					Prediction.m_aaPos[ClientId][0], Closest) &&
					distance(Prediction.m_aaPos[ClientId][0], Closest) <
						CCharacterCore::PhysicalSize() + 2.0f)
				{
					HitsIgnoredFriend = true;
					break;
				}
			}
			if(HitsIgnoredFriend)
				continue;
			const float CandidateAngle = std::abs(Offset);
			if(CandidateAngle < BestAngle)
			{
				BestAngle = CandidateAngle;
				BestDirection = Direction;
			}
		}
		if(BestAngle == 1e18f)
			return false;
		pInput->m_TargetX = round_to_int(BestDirection.x * 1000.0f);
		pInput->m_TargetY = round_to_int(BestDirection.y * 1000.0f);
		if(pInput->m_TargetX == 0 && pInput->m_TargetY == 0)
			pInput->m_TargetX = 1;
		return true;
	}

	const bool LockedHasSamePriority = LockedTarget.Valid() &&
		(!g_Config.m_ClZzAimbotPreferFrozen || !BestFrozenTarget.Valid() || LockedTarget.m_Frozen);
	if(LockedHasSamePriority && LockedTarget.m_Score <= Selected.m_Score + TARGET_LOCK_TOLERANCE)
		Selected = LockedTarget;

	m_aLockedTarget[DummyIndex] = Selected.m_ClientId;
	float CursorDistance = 0.0f;
	if(!g_Config.m_ClZzAimbotSilent)
	{
		CursorDistance = length(GameClient()->m_Controls.m_aMousePos[DummyIndex]);
		if(CursorDistance < 1.0f)
			CursorDistance = length(vec2((float)pInput->m_TargetX, (float)pInput->m_TargetY));
		CursorDistance = std::max(CursorDistance, 1.0f);
	}
	pInput->m_TargetX = round_to_int(Selected.m_AimDirection.x * 1000.0f);
	pInput->m_TargetY = round_to_int(Selected.m_AimDirection.y * 1000.0f);
	if(pInput->m_TargetX == 0 && pInput->m_TargetY == 0)
		pInput->m_TargetX = 1;
	if(!g_Config.m_ClZzAimbotSilent)
		GameClient()->m_Controls.m_aMousePos[DummyIndex] = Selected.m_AimDirection * CursorDistance;
	return true;
}
