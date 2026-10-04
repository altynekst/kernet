#include "zz_hook_aimbot.h"

#include <algorithm>
#include <cmath>

#include <base/math.h>

#include <engine/shared/config.h>

#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>

namespace
{
constexpr float TARGET_LOCK_TOLERANCE = 0.12f;
constexpr float MIN_AIM_DISTANCE = 0.001f;
constexpr int HOOK_ANGLE_SAMPLES = 16;

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

bool CZZHookAimbot::BuildTarget(int ClientId, int DummyIndex, const vec2 &LocalPos, const vec2 &UserAim, float HalfFov, float HookLength, STarget &Target) const
{
	const CGameClient::CClientData &ClientData = GameClient()->m_aClients[ClientId];
	const vec2 CurrentPos = ClientData.m_RegularPredicted.m_Pos;
	const float CenterDistance = distance(LocalPos, CurrentPos);
	if(CenterDistance < MIN_AIM_DISTANCE)
		return false;

	const float HookSpeed = std::max(1.0f, (float)GameClient()->m_aTuning[DummyIndex].m_HookFireSpeed);
	const float LeadTicks = std::clamp(CenterDistance / HookSpeed, 0.0f, 4.0f);
	const vec2 PredictedPos = CurrentPos + ClientData.m_RegularPredicted.m_Vel * (LeadTicks * 0.65f);
	const float TeeRadius = CCharacterCore::PhysicalSize();
	const float HookHitRadius = TeeRadius + 2.0f;
	const float MaxAimDistance = HookLength + HookHitRadius;

	vec2 BestDirection = vec2(1.0f, 0.0f);
	float BestPointAngle = 1e18f;
	float BestPointDistance = 1e18f;
	bool Found = false;

	const auto TryDirection = [&](const vec2 &Direction, const vec2 &TargetPos) {
		const float TargetDistance = distance(LocalPos, TargetPos);
		if(TargetDistance < MIN_AIM_DISTANCE || TargetDistance > MaxAimDistance)
			return;

		const float Angle = AimAngle(UserAim, Direction);
		if(Angle > HalfFov)
			return;

		// Match the server hook path: it starts in front of the tee, stops on
		// hook collision, and can grab a player before attaching to the wall.
		const vec2 HookStart = LocalPos + Direction * TeeRadius * 1.5f;
		const vec2 HookEnd = LocalPos + Direction * HookLength;
		if(dot(HookEnd - HookStart, Direction) <= 0.0f)
			return;

		vec2 RayEnd = HookEnd;
		int TeleNr = 0;
		Collision()->IntersectLineTeleHook(HookStart, HookEnd, &RayEnd, nullptr, &TeleNr);
		const float RayLength = std::max(0.0f, dot(RayEnd - HookStart, Direction));

		const vec2 ToTarget = TargetPos - HookStart;
		const float AlongRay = dot(ToTarget, Direction);
		const float RadiusSq = HookHitRadius * HookHitRadius;
		float EntryDistance = 0.0f;
		if(AlongRay < 0.0f)
		{
			if(dot(ToTarget, ToTarget) >= RadiusSq)
				return;
		}
		else
		{
			const float PerpendicularSq = std::max(0.0f, dot(ToTarget, ToTarget) - AlongRay * AlongRay);
			if(PerpendicularSq >= RadiusSq)
				return;
			EntryDistance = AlongRay - std::sqrt(RadiusSq - PerpendicularSq);
		}
		if(EntryDistance > RayLength + 0.5f)
			return;

		if(Angle < BestPointAngle || (std::abs(Angle - BestPointAngle) < 0.0005f && TargetDistance < BestPointDistance))
		{
			BestDirection = Direction;
			BestPointAngle = Angle;
			BestPointDistance = TargetDistance;
			Found = true;
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
			TryDirection(Rotate(CenterDirection, Offset), TargetPos);
		}
	};

	ScanTargetAngles(PredictedPos);
	if(distance(PredictedPos, CurrentPos) > 1.0f)
		ScanTargetAngles(CurrentPos);

	if(!Found)
		return false;

	vec2 CenterDirection = PredictedPos - LocalPos;
	if(length(CenterDirection) < MIN_AIM_DISTANCE)
		CenterDirection = CurrentPos - LocalPos;
	CenterDirection = normalize(CenterDirection);
	const float FovScale = std::max(HalfFov, 0.01f);
	const float CenterAngle = AimAngle(UserAim, CenterDirection);
	Target.m_ClientId = ClientId;
	Target.m_AimDirection = BestDirection;
	Target.m_Frozen = IsFrozen(ClientId, DummyIndex);
	Target.m_Score = CenterAngle / FovScale * 0.70f + BestPointAngle / FovScale * 0.24f + BestPointDistance / std::max(HookLength, 1.0f) * 0.06f;
	return true;
}

bool CZZHookAimbot::Apply(CNetObj_PlayerInput *pInput, int DummyIndex, const vec2 &LocalPos, const vec2 &UserAim, bool InputBlocked)
{
	DummyIndex = std::clamp(DummyIndex, 0, NUM_DUMMIES - 1);
	if(InputBlocked || !g_Config.m_ClZzAimbotEnabled || !pInput->m_Hook)
	{
		ClearLock(DummyIndex);
		return false;
	}

	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	if(LocalId < 0 || LocalId >= MAX_CLIENTS)
	{
		ClearLock(DummyIndex);
		return false;
	}

	const float HalfFov = std::clamp((float)g_Config.m_ClZzAimbotFov, 1.0f, 180.0f) * (pi / 360.0f);
	const float HookLength = (float)GameClient()->m_aTuning[DummyIndex].m_HookLength;
	vec2 AimDirection = UserAim;
	if(length(AimDirection) < MIN_AIM_DISTANCE)
		AimDirection = vec2(1.0f, 0.0f);
	else
		AimDirection = normalize(AimDirection);
	CCharacter *pLocalPredCharacter =
		GameClient()->m_RegularPredictedWorld.GetCharacterById(LocalId);
	STarget BestTarget;
	STarget BestFrozenTarget;
	STarget LockedTarget;

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(ClientId == LocalId || !GameClient()->m_Snap.m_aCharacters[ClientId].m_Active)
			continue;
		if(g_Config.m_ClZzAimbotIgnoreFriends && GameClient()->m_aClients[ClientId].m_Friend)
			continue;
		if(pLocalPredCharacter && !pLocalPredCharacter->CanCollide(ClientId))
			continue;

		STarget Candidate;
		if(!BuildTarget(ClientId, DummyIndex, LocalPos, AimDirection, HalfFov,
			HookLength, Candidate))
		{
			// Keep an already acquired target through a small angular dead zone.
			// This prevents rapid target loss when prediction moves a tee across
			// the exact FOV edge for one tick.
			if(ClientId != m_aLockedTarget[DummyIndex] ||
				!BuildTarget(ClientId, DummyIndex, LocalPos, AimDirection,
					HalfFov * 1.15f, HookLength, Candidate))
				continue;
		}
		if(!BestTarget.Valid() || Candidate.m_Score < BestTarget.m_Score)
			BestTarget = Candidate;
		if(Candidate.m_Frozen && (!BestFrozenTarget.Valid() || Candidate.m_Score < BestFrozenTarget.m_Score))
			BestFrozenTarget = Candidate;
		if(ClientId == m_aLockedTarget[DummyIndex])
			LockedTarget = Candidate;
	}

	STarget Selected = g_Config.m_ClZzAimbotPreferFrozen && BestFrozenTarget.Valid() ? BestFrozenTarget : BestTarget;
	if(!Selected.Valid())
	{
		ClearLock(DummyIndex);
		return false;
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
