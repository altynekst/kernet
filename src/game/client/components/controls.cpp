/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "controls.h"

#include <base/math.h>
#include <base/time.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/client/components/camera.h>
#include <game/client/components/chat.h>
#include <game/client/components/menus.h>
#include <game/client/components/scoreboard.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/collision.h>

CControls::CControls()
{
	mem_zero(&m_aLastData, sizeof(m_aLastData));
	mem_zero(&m_aFastInput, sizeof(m_aFastInput));
	std::fill(std::begin(m_aMousePos), std::end(m_aMousePos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aMousePosOnAction), std::end(m_aMousePosOnAction), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aTargetPos), std::end(m_aTargetPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aMouseInputType), std::end(m_aMouseInputType), EMouseInputType::ABSOLUTE);
}

void CControls::OnReset()
{
	for(int Dummy = 0; Dummy < NUM_DUMMIES; Dummy++)
	{
		ResetInput(Dummy);
		m_aFastInput[Dummy] = m_aInputData[Dummy];
	}
	std::fill(std::begin(m_aUiInputFrozen), std::end(m_aUiInputFrozen), false);
	m_FastInputHookAction = false;
	m_FastInputFireAction = false;

	for(int &AmmoCount : m_aAmmoCount)
		AmmoCount = 0;

	m_LastSendTime = 0;
	m_ZzFngDecisionValid = false;
	m_ZzFngDecisionTick = -1;
}

void CControls::ResetInput(int Dummy)
{
	if(Dummy < 0 || Dummy >= NUM_DUMMIES)
		return;

	m_aLastData[Dummy].m_Direction = 0;
	// simulate releasing the fire button
	if((m_aLastData[Dummy].m_Fire & 1) != 0)
		m_aLastData[Dummy].m_Fire++;
	m_aLastData[Dummy].m_Fire &= INPUT_STATE_MASK;
	m_aLastData[Dummy].m_Jump = 0;
	m_aInputData[Dummy] = m_aLastData[Dummy];

	m_aInputDirectionLeft[Dummy] = 0;
	m_aInputDirectionRight[Dummy] = 0;
	m_aSaveInBlock[Dummy] = 0;
	m_aRapidFireLastTick[Dummy] = 0;
	m_aRapidFireActive[Dummy] = false;
	m_aRapidFireWasPressed[Dummy] = false;
}

void CControls::OnPlayerDeath()
{
	for(int &AmmoCount : m_aAmmoCount)
		AmmoCount = 0;
}

struct CInputState
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
};

void CControls::ConKeyInputState(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if(pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active)
		return;

	*pState->m_apVariables[g_Config.m_ClDummy] = pResult->GetInteger(0);
}

void CControls::ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if((pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active) || pState->m_pControls->GameClient()->m_Spectator.IsActive())
		return;

	int *pVariable = pState->m_apVariables[g_Config.m_ClDummy];
	if(((*pVariable) & 1) != pResult->GetInteger(0))
		(*pVariable)++;
	*pVariable &= INPUT_STATE_MASK;
}

struct CInputSet
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
	int m_Value;
};

struct CInputToggle
{
	int *m_pVariable;
};

static void ConKeyToggle(IConsole::IResult *pResult, void *pUserData)
{
	CInputToggle *pToggle = (CInputToggle *)pUserData;
	if(pResult->GetInteger(0))
		*pToggle->m_pVariable ^= 1;
}

struct CInputToggleOnce
{
	int *m_pVariable;
	bool m_WasDown;
};

static void ConKeyToggleOnce(IConsole::IResult *pResult, void *pUserData)
{
	CInputToggleOnce *pToggle = (CInputToggleOnce *)pUserData;
	const bool Down = pResult->GetInteger(0) != 0;
	if(Down && !pToggle->m_WasDown)
		*pToggle->m_pVariable ^= 1;
	pToggle->m_WasDown = Down;
}

struct CFentBotWaypointKey
{
	CControls *m_pControls;
	CGameClient *m_pGameClient;
	bool m_WasDown;
};

static void ConKeyFentBotWaypoint(IConsole::IResult *pResult, void *pUserData)
{
	CFentBotWaypointKey *pKey = (CFentBotWaypointKey *)pUserData;
	const bool Down = pResult->GetInteger(0) != 0;
	if(Down && !pKey->m_WasDown)
	{
		CGameClient *pGameClient = pKey->m_pGameClient;
		if(pGameClient->FentBotHasWaypoint())
			pGameClient->FentBotClearWaypoint();
		else
		{
			if(pGameClient->m_Snap.m_pLocalCharacter)
			{
				const int LocalId = pGameClient->m_Snap.m_LocalClientId;
				const vec2 LocalPos = pGameClient->m_aClients[LocalId].m_RegularPredicted.m_Pos;
				vec2 Aim((float)pGameClient->m_Controls.m_aInputData[g_Config.m_ClDummy].m_TargetX, (float)pGameClient->m_Controls.m_aInputData[g_Config.m_ClDummy].m_TargetY);
				if(length(Aim) < 0.001f)
					Aim = vec2(1.0f, 0.0f);
				Aim = normalize(Aim);
				vec2 HitPos;
				pGameClient->Collision()->IntersectLine(LocalPos, LocalPos + Aim * 2000.0f, &HitPos, nullptr);
				pGameClient->FentBotSetWaypoint(HitPos);
			}
		}
	}
	pKey->m_WasDown = Down;
}

void CControls::ConKeyInputSet(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	if(pResult->GetInteger(0))
	{
		*pSet->m_apVariables[g_Config.m_ClDummy] = pSet->m_Value;
	}
}

void CControls::ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	ConKeyInputCounter(pResult, pSet);
	pSet->m_pControls->m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = 0;
}

void CControls::OnConsoleInit()
{
	// game commands
	{
		static CInputState s_State = {this, {&m_aInputDirectionLeft[0], &m_aInputDirectionLeft[1], &m_aInputDirectionLeft[2]}};
		Console()->Register("+left", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move left");
	}
	{
		static CInputState s_State = {this, {&m_aInputDirectionRight[0], &m_aInputDirectionRight[1], &m_aInputDirectionRight[2]}};
		Console()->Register("+right", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move right");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Jump, &m_aInputData[1].m_Jump, &m_aInputData[2].m_Jump}};
		Console()->Register("+jump", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Jump");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Hook, &m_aInputData[1].m_Hook, &m_aInputData[2].m_Hook}};
		Console()->Register("+hook", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Hook");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Fire, &m_aInputData[1].m_Fire, &m_aInputData[2].m_Fire}};
		Console()->Register("+fire", "", CFGFLAG_CLIENT, ConKeyInputCounter, &s_State, "Fire");
	}
	{
		static CInputState s_State = {this, {&m_aShowHookColl[0], &m_aShowHookColl[1], &m_aShowHookColl[2]}};
		Console()->Register("+showhookcoll", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Show Hook Collision");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon, &m_aInputData[2].m_WantedWeapon}, 1};
		Console()->Register("+weapon1", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to hammer");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon, &m_aInputData[2].m_WantedWeapon}, 2};
		Console()->Register("+weapon2", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to gun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon, &m_aInputData[2].m_WantedWeapon}, 3};
		Console()->Register("+weapon3", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to shotgun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon, &m_aInputData[2].m_WantedWeapon}, 4};
		Console()->Register("+weapon4", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to grenade");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon, &m_aInputData[2].m_WantedWeapon}, 5};
		Console()->Register("+weapon5", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to laser");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_NextWeapon, &m_aInputData[1].m_NextWeapon, &m_aInputData[2].m_NextWeapon}, 0};
		Console()->Register("+nextweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to next weapon");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_PrevWeapon, &m_aInputData[1].m_PrevWeapon, &m_aInputData[2].m_PrevWeapon}, 0};
		Console()->Register("+prevweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to previous weapon");
	}

	// KernelNet
	{
		static CInputToggleOnce s_Toggle = {&g_Config.m_ClZzAimbotEnabled, false};
		static CInputToggle s_ToggleLegacy = {&g_Config.m_ClZzAimbotEnabled};
		// Note: for +commands, the console injects the stroke argument before parsing,
		// so the params string must be empty.
		Console()->Register("+zz_aimbot_toggle", "", CFGFLAG_CLIENT, ConKeyToggleOnce, &s_Toggle, "KernelNet: toggle aimbot");
		Console()->Register("zz_aimbot_toggle", "", CFGFLAG_CLIENT, ConKeyToggle, &s_ToggleLegacy, "KernelNet: toggle aimbot (legacy)");
	}
	{
		static CInputToggleOnce s_Toggle = {&g_Config.m_ClZzAvoidEnabled, false};
		static CInputToggle s_ToggleLegacy = {&g_Config.m_ClZzAvoidEnabled};
		Console()->Register("+zz_avoid_toggle", "", CFGFLAG_CLIENT, ConKeyToggleOnce, &s_Toggle, "KernelNet: toggle avoid freeze");
		Console()->Register("zz_avoid_toggle", "", CFGFLAG_CLIENT, ConKeyToggle, &s_ToggleLegacy, "KernelNet: toggle avoid freeze (legacy)");
	}
	{
		static CFentBotWaypointKey s_Key = {this, GameClient(), false};
		Console()->Register("+zz_fentbot_waypoint", "", CFGFLAG_CLIENT, ConKeyFentBotWaypoint, &s_Key, "KernelNet: set/clear FentBot waypoint");
	}
	{
		static CInputState s_State = {this, {&m_aSaveInBlock[0], &m_aSaveInBlock[1], &m_aSaveInBlock[2]}};
		Console()->Register("+zz_tile_aimbot", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "KernelNet: hook a hookable tile in your FOV (hold)");
		Console()->Register("+zz_save_in_block", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "KernelNet: hook a hookable tile in your FOV (hold) (legacy alias)");
	}

	// Re-apply saved keybind (if any).
	if(g_Config.m_ClZzAimbotKey >= KEY_FIRST && g_Config.m_ClZzAimbotKey < KEY_LAST)
		GameClient()->m_Binds.Bind(g_Config.m_ClZzAimbotKey, "toggle cl_zz_aimbot_enabled 0 1", false, g_Config.m_ClZzAimbotMod);
	if(g_Config.m_ClZzAvoidToggleKey >= KEY_FIRST && g_Config.m_ClZzAvoidToggleKey < KEY_LAST)
		GameClient()->m_Binds.Bind(g_Config.m_ClZzAvoidToggleKey, "toggle cl_zz_avoid_enabled 0 1", false, g_Config.m_ClZzAvoidToggleMod);
	if(g_Config.m_ClZzFentBotWaypointKey >= KEY_FIRST && g_Config.m_ClZzFentBotWaypointKey < KEY_LAST)
		GameClient()->m_Binds.Bind(g_Config.m_ClZzFentBotWaypointKey, "+zz_fentbot_waypoint", false, g_Config.m_ClZzFentBotWaypointMod);
}

void CControls::OnMessage(int Msg, void *pRawMsg)
{
	if(Msg == NETMSGTYPE_SV_WEAPONPICKUP)
	{
		CNetMsg_Sv_WeaponPickup *pMsg = (CNetMsg_Sv_WeaponPickup *)pRawMsg;
		if(g_Config.m_ClAutoswitchWeapons)
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = pMsg->m_Weapon + 1;
		// We don't really know ammo count, until we'll switch to that weapon, but any non-zero count will suffice here
		m_aAmmoCount[maximum(0, pMsg->m_Weapon % NUM_WEAPONS)] = 10;
	}
}

int CControls::SnapInput(int *pData)
{
	// update player state
	if(GameClient()->m_Chat.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_CHATTING;
	else if(GameClient()->m_Menus.IsActive() || g_Config.m_ClZzClickGui)
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_IN_MENU;
	else
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_PLAYING;

	if(GameClient()->m_Scoreboard.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

	if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Controls.m_aShowHookColl[g_Config.m_ClDummy])
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_AIM;

	if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Camera.CamType() == CCamera::CAMTYPE_SPEC)
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SPEC_CAM;

	switch(m_aMouseInputType[g_Config.m_ClDummy])
	{
	case CControls::EMouseInputType::AUTOMATED:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE;
		break;
	case CControls::EMouseInputType::ABSOLUTE:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE | PLAYERFLAG_INPUT_MANUAL;
		break;
	case CControls::EMouseInputType::RELATIVE:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_MANUAL;
		break;
	}

	// TClient
	if(g_Config.m_TcHideChatBubbles && Client()->RconAuthed())
		for(auto &InputData : m_aInputData)
			InputData.m_PlayerFlags &= ~PLAYERFLAG_CHATTING;

	if(g_Config.m_TcNameplatePingCircle)
		for(auto &InputData : m_aInputData)
			InputData.m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

	bool Send = m_aLastData[g_Config.m_ClDummy].m_PlayerFlags != m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	m_aLastData[g_Config.m_ClDummy].m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	// Release gameplay actions while a text/menu UI owns the input. ClickGUI is
	// included here as well: it captures keys and therefore must not leave an
	// already-held movement or hook action running in the prediction buffer.
	if(!(m_aInputData[g_Config.m_ClDummy].m_PlayerFlags & PLAYERFLAG_PLAYING))
	{
		if(!GameClient()->m_GameInfo.m_BugDDRaceInput)
		{
			if(!m_aUiInputFrozen[g_Config.m_ClDummy])
			{
				ResetInput(g_Config.m_ClDummy);
				// Fast input is also used for over-run prediction. Align it with
				// the released input once, otherwise it can keep a stale action.
				m_aFastInput[g_Config.m_ClDummy] = m_aInputData[g_Config.m_ClDummy];
				m_FastInputHookAction = false;
				m_FastInputFireAction = false;
				m_aUiInputFrozen[g_Config.m_ClDummy] = true;
			}
		}
		else
		{
			m_aUiInputFrozen[g_Config.m_ClDummy] = false;
		}

		mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

		// set the target anyway though so that we can keep seeing our surroundings,
		// even if chat or menu are activated
		vec2 Pos = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];
		if(g_Config.m_TcScaleMouseDistance && !GameClient()->m_Snap.m_SpecInfo.m_Active)
		{
			const int MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
			if(MaxDistance > 5 && MaxDistance < 1000) // Don't scale if angle bind or reduces precision
				Pos *= 1000.0f / (float)MaxDistance;
		}
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)Pos.x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)Pos.y;

		if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
			m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;

		// Keep acknowledgements and time-sync replies fresh while the UI owns
		// gameplay input. A one-second keepalive made the server see an
		// acknowledged snapshot that was almost one second old, which looked like
		// a ~1000 ms ping whenever chat, a menu, or ClickGUI was open.
		Send = Send || time_get() > m_LastSendTime + time_freq() / 25;
	}
	else
	{
		m_aUiInputFrozen[g_Config.m_ClDummy] = false;

		// TClient
		vec2 Pos;
		if(g_Config.m_ClSubTickAiming && m_aMousePosOnAction[g_Config.m_ClDummy] != vec2(0.0f, 0.0f))
		{
			Pos = GameClient()->m_Controls.m_aMousePosOnAction[g_Config.m_ClDummy];
			m_aMousePosOnAction[g_Config.m_ClDummy] = vec2(0.0f, 0.0f);
		}
		else
			Pos = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];

		m_FastInputHookAction = false;
		m_FastInputFireAction = false;

		if(g_Config.m_TcScaleMouseDistance && !GameClient()->m_Snap.m_SpecInfo.m_Active)
		{
			const int MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
			if(MaxDistance > 5 && MaxDistance < 1000) // Don't scale if angle bind or reduces precision
				Pos *= 1000.0f / (float)MaxDistance;
		}
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)Pos.x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)Pos.y;

		if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
			m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;

		// set direction
		m_aInputData[g_Config.m_ClDummy].m_Direction = 0;
		if(m_aInputDirectionLeft[g_Config.m_ClDummy] && !m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = -1;
		if(!m_aInputDirectionLeft[g_Config.m_ClDummy] && m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = 1;

		// KernelNet: FNG laser assist. Shared trigger/auto logic keeps the shot valid:
		// ready weapon, no laser-hit-disable, wall path exists, and the first tee on the ray is an enemy.
		static bool s_aFngLaserReleasePending[NUM_DUMMIES] = {false, false, false};
		static int s_aFngLaserInjectedFire[NUM_DUMMIES] = {-1, -1, -1};
		static int s_aFngLaserSwitchKeepTicks[NUM_DUMMIES] = {0, 0, 0};
		static int s_aFngLaserScanStep[NUM_DUMMIES] = {0, 0, 0};
		const int FngDummyIndex = g_Config.m_ClDummy;
		m_ZzFngDecisionValid = false;
		if((g_Config.m_ClZzFngTriggerLaser || g_Config.m_ClZzFngAutoLaser || g_Config.m_ClZzFngAimOnFire) && FngDummyIndex == 0 && GameClient()->m_Snap.m_pLocalCharacter && !GameClient()->m_Snap.m_SpecInfo.m_Active)
		{
			int LocalId = GameClient()->m_aLocalIds[FngDummyIndex];
			if(LocalId < 0)
				LocalId = GameClient()->m_Snap.m_LocalClientId;
			if(LocalId >= 0)
			{
				const CNetObj_Character *pLocalChar = GameClient()->m_Snap.m_pLocalCharacter;
				CCharacter *pLocalPredChar = GameClient()->m_RegularPredictedWorld.GetCharacterById(LocalId);
				const bool FngServerRules = GameClient()->m_GameInfo.m_EntitiesFNG || GameClient()->m_GameInfo.m_PredictFNG || str_find_nocase(GameClient()->m_GameInfo.m_aGameType, "fng") != nullptr;
				const bool LegacyFng2Server = str_find_nocase(GameClient()->m_GameInfo.m_aGameType, "fng2") != nullptr;
				const bool AimOnFire = g_Config.m_ClZzFngAimOnFire != 0;
				const bool UseAutoAim = g_Config.m_ClZzFngAutoLaser || AimOnFire;
				const bool LaserSelected = pLocalChar->m_Weapon == WEAPON_LASER;
				const bool LaserReady = !pLocalPredChar || pLocalPredChar->GetReloadTimer() <= 0;
				// FNG2 keeps unlimited ammo as -1, but its legacy snapshot deliberately sends 0.
				const bool LaserHasAmmo = LegacyFng2Server || !LaserSelected || pLocalChar->m_AmmoCount != 0;
				const bool LaserCanHit = !GameClient()->m_aClients[LocalId].m_LaserHitDisabled;
				bool SwitchRequested = false;
				bool ReleasedThisTick = false;
				bool InjectedAutoShot = false;
				bool InjectedTriggerShot = false;

				if(g_Config.m_ClZzFngAutoLaser && !AimOnFire && !LaserSelected && s_aFngLaserSwitchKeepTicks[FngDummyIndex] > 0)
				{
					m_aInputData[FngDummyIndex].m_WantedWeapon = WEAPON_LASER + 1;
					SwitchRequested = true;
					s_aFngLaserSwitchKeepTicks[FngDummyIndex]--;
				}

				if(s_aFngLaserReleasePending[FngDummyIndex] &&
					(m_aInputData[FngDummyIndex].m_Fire & 1) != 0 &&
					m_aInputData[FngDummyIndex].m_Fire == s_aFngLaserInjectedFire[FngDummyIndex])
				{
					m_aInputData[FngDummyIndex].m_Fire = (m_aInputData[FngDummyIndex].m_Fire + 1) & INPUT_STATE_MASK;
					s_aFngLaserReleasePending[FngDummyIndex] = false;
					s_aFngLaserInjectedFire[FngDummyIndex] = -1;
					ReleasedThisTick = true;
				}

				const vec2 LocalPos = GameClient()->m_aClients[LocalId].m_RegularPredicted.m_Pos;
				const int LocalTeam = GameClient()->m_aClients[LocalId].m_Team;
				const bool TeamPlay = GameClient()->IsTeamPlay();
				const float Reach = std::max((float)GameClient()->m_aClients[LocalId].m_Predicted.m_Tuning.m_LaserReach, 800.0f);
				const float TriggerReach = std::min(Reach, (float)std::clamp(g_Config.m_ClZzFngTriggerDistance, 1, 26) * 32.0f);
				const float BounceCost = (float)GameClient()->m_aClients[LocalId].m_Predicted.m_Tuning.m_LaserBounceCost;
				const int BounceDelayTicks = std::clamp(round_to_int((float)GameClient()->m_aClients[LocalId].m_Predicted.m_Tuning.m_LaserBounceDelay * (float)Client()->GameTickSpeed() / 1000.0f), 0, 10);
				const float HitRadius = CCharacterCore::PhysicalSize();
				const float HitRadius2 = HitRadius * HitRadius;
				const float AimOffsetRadius = CCharacterCore::PhysicalSize() * 0.68f;
				static const vec2 s_aAimOffsets[] = {
					vec2(1.0f, 0.0f),
					vec2(-1.0f, 0.0f),
					vec2(0.0f, 1.0f),
					vec2(0.0f, -1.0f),
					normalize(vec2(1.0f, 1.0f)),
					normalize(vec2(-1.0f, 1.0f)),
					normalize(vec2(1.0f, -1.0f)),
					normalize(vec2(-1.0f, -1.0f)),
				};

				vec2 Aim((float)m_aInputData[FngDummyIndex].m_TargetX, (float)m_aInputData[FngDummyIndex].m_TargetY);
				const float AimLen = length(Aim);
				const vec2 CurAim = AimLen > 0.001f ? Aim * (1.0f / AimLen) : vec2(1.0f, 0.0f);
				const bool FngPingMode = g_Config.m_ClZzFngPingMode != 0;
				const bool FngFastMode = g_Config.m_ClZzFngFastMode != 0;
				const int FngAimMode = std::clamp(g_Config.m_ClZzFngAimMode, 0, 2);
				const int AimOffsetCount = FngAimMode == 0 ? 4 : 8;
				const int PingMinTicks = FngAimMode == 0 ? 2 : 3;
				const int PingMaxTicks = FngAimMode == 0 ? 8 : (FngAimMode == 1 ? 11 : 14);
				const int PingCompTicks = FngPingMode ? std::clamp((Client()->GetPredictionTime() * Client()->GameTickSpeed() + 999) / 1000, PingMinTicks, PingMaxTicks) : 0;

				auto IsFrozen = [&](int ClientId) {
					if(ClientId < 0 || ClientId >= MAX_CLIENTS)
						return false;
					// Legacy FNG servers represent frozen tees with the ninja weapon instead of DDNet freeze data.
					if(FngServerRules && GameClient()->m_Snap.m_aCharacters[ClientId].m_Active &&
						GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur.m_Weapon == WEAPON_NINJA)
						return true;
					if(GameClient()->m_aClients[ClientId].m_DeepFrozen || GameClient()->m_aClients[ClientId].m_LiveFrozen)
						return true;
					if(GameClient()->m_aClients[ClientId].m_FreezeEnd > Client()->GameTick(FngDummyIndex))
						return true;
					return GameClient()->m_aClients[ClientId].m_Predicted.m_IsInFreeze != 0;
				};

				auto IsEnemy = [&](int ClientId) {
					if(ClientId == LocalId || IsFrozen(ClientId))
						return false;
					const int OtherTeam = GameClient()->m_aClients[ClientId].m_Team;
					if(OtherTeam == TEAM_SPECTATORS)
						return false;
					if(!TeamPlay)
						return true;
					if((LocalTeam == TEAM_RED || LocalTeam == TEAM_BLUE) && (OtherTeam == TEAM_RED || OtherTeam == TEAM_BLUE) && OtherTeam == LocalTeam)
						return false;
					if(FngServerRules)
						return true;
					return true;
				};

				auto AddTargetCandidate = [&](vec2 *pPositions, int &NumPositions, int MaxPositions, const vec2 &Pos) {
					for(int i = 0; i < NumPositions; i++)
					{
						const vec2 Delta = pPositions[i] - Pos;
						if(dot(Delta, Delta) < 1.0f)
							return;
					}
					if(NumPositions < MaxPositions)
						pPositions[NumPositions++] = Pos;
				};

				auto AddBufferedPredPos = [&](int ClientId, int TicksAhead, vec2 *pPositions, int &NumPositions, int MaxPositions) {
					const float CombinedIntra = Client()->PredIntraGameTick(FngDummyIndex) + (float)TicksAhead;
					const int WholeTicks = (int)CombinedIntra;
					const float FinalIntra = CombinedIntra - (float)WholeTicks;
					const int FinalTick = Client()->PredGameTick(FngDummyIndex) + WholeTicks;
					if(FinalTick <= 0)
						return;
					const int PrevIndex = (FinalTick - 1) % 200;
					const int CurIndex = FinalTick % 200;
					if(GameClient()->m_aClients[ClientId].m_aPredTick[PrevIndex] != FinalTick - 1 ||
						GameClient()->m_aClients[ClientId].m_aPredTick[CurIndex] != FinalTick)
						return;
					AddTargetCandidate(pPositions, NumPositions, MaxPositions, mix(GameClient()->m_aClients[ClientId].m_aPredPos[PrevIndex], GameClient()->m_aClients[ClientId].m_aPredPos[CurIndex], FinalIntra));
				};

				auto AddVelocityPredPos = [&](int ClientId, int TicksAhead, vec2 *pPositions, int &NumPositions, int MaxPositions) {
					if(TicksAhead <= 0)
						return;
					vec2 Velocity = GameClient()->m_aClients[ClientId].m_Predicted.m_Vel;
					const vec2 PosDelta = GameClient()->m_aClients[ClientId].m_Predicted.m_Pos - GameClient()->m_aClients[ClientId].m_PrevPredicted.m_Pos;
					if(length(Velocity) < 0.001f && length(PosDelta) > 0.001f)
						Velocity = PosDelta;
					vec2 Offset = Velocity * (float)TicksAhead;
					const float OffsetLen = length(Offset);
					const float MaxOffset = FngAimMode == 0 ? (FngPingMode ? 220.0f : (FngFastMode ? 160.0f : 110.0f)) :
								     (FngAimMode == 1 ? (FngPingMode ? 300.0f : (FngFastMode ? 220.0f : 150.0f)) :
											       (FngPingMode ? 360.0f : (FngFastMode ? 240.0f : 180.0f)));
					if(OffsetLen > MaxOffset)
						Offset *= MaxOffset / OffsetLen;
					AddTargetCandidate(pPositions, NumPositions, MaxPositions, GameClient()->m_aClients[ClientId].m_RegularPredicted.m_Pos + Offset);
				};

				auto BuildTargetCandidates = [&](int ClientId, vec2 *pPositions, int &NumPositions, int MaxPositions, int ExtraTicks) {
					NumPositions = 0;
					const int TravelTicks = std::clamp(ExtraTicks, 0, 18);
					if(TravelTicks <= 0)
					{
						// RegularPredicted is already advanced to PredGameTick, which includes network prediction.
						AddTargetCandidate(pPositions, NumPositions, MaxPositions, GameClient()->m_aClients[ClientId].m_RegularPredicted.m_Pos);
						return;
					}

					// Only bounced lasers need additional lead for the time spent travelling between bounces.
					AddBufferedPredPos(ClientId, TravelTicks, pPositions, NumPositions, MaxPositions);
					if(NumPositions == 0)
						AddVelocityPredPos(ClientId, TravelTicks, pPositions, NumPositions, MaxPositions);
					if(NumPositions == 0)
						AddTargetCandidate(pPositions, NumPositions, MaxPositions, GameClient()->m_aClients[ClientId].m_RegularPredicted.m_Pos);
				};

				int aActiveClientIds[MAX_CLIENTS];
				int NumActiveClients = 0;
				for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
				{
					if(ClientId != LocalId && GameClient()->m_Snap.m_aCharacters[ClientId].m_Active)
						aActiveClientIds[NumActiveClients++] = ClientId;
				}

				struct SCachedHitCandidates
				{
					int m_Epoch;
					int m_NumPositions;
					vec2 m_aPositions[12];
				};
				static SCachedHitCandidates s_aaaHitCandidateCache[NUM_DUMMIES][MAX_CLIENTS][19] = {};
				static int s_aHitCandidateEpoch[NUM_DUMMIES] = {};
				const int HitCandidateEpoch = ++s_aHitCandidateEpoch[FngDummyIndex];
				auto HitCandidateCacheKey = [&](int ExtraTicks) {
					return std::clamp(ExtraTicks, 0, 18);
				};
				auto GetHitCandidates = [&](int ClientId, int ExtraTicks) -> SCachedHitCandidates & {
					const int CacheKey = HitCandidateCacheKey(ExtraTicks);
					SCachedHitCandidates &Cache = s_aaaHitCandidateCache[FngDummyIndex][ClientId][CacheKey];
					if(Cache.m_Epoch != HitCandidateEpoch)
					{
						Cache.m_Epoch = HitCandidateEpoch;
						BuildTargetCandidates(ClientId, Cache.m_aPositions, Cache.m_NumPositions, std::size(Cache.m_aPositions), ExtraTicks);
					}
					return Cache;
				};

				auto SegmentFirstHitIsEnemy = [&](const vec2 &From, const vec2 &To, float *pOutHitDist = nullptr, int ExtraTicks = 0) {
					const vec2 Ray = To - From;
					const float RayLen2 = dot(Ray, Ray);
					if(RayLen2 < 0.001f)
						return false;
					const float RayLen = sqrtf(RayLen2);
					const vec2 Dir = Ray * (1.0f / RayLen);
					float BestT = RayLen + 1.0f;
					int BestClientId = -1;
					for(int ActiveIndex = 0; ActiveIndex < NumActiveClients; ActiveIndex++)
					{
						const int ClientId = aActiveClientIds[ActiveIndex];
						const SCachedHitCandidates &Cache = GetHitCandidates(ClientId, ExtraTicks);
						for(int PosIndex = 0; PosIndex < Cache.m_NumPositions; PosIndex++)
						{
							const vec2 TargetPos = Cache.m_aPositions[PosIndex];
							const float T = dot(TargetPos - From, Dir);
							if(T < 0.0f || T > RayLen || T >= BestT)
								continue;
							const vec2 Closest = From + Dir * T;
							const vec2 Delta = TargetPos - Closest;
							if(dot(Delta, Delta) <= HitRadius2)
							{
								BestT = T;
								BestClientId = ClientId;
							}
						}
					}
					if(pOutHitDist)
						*pOutHitDist = BestT;
					return BestClientId >= 0 && IsEnemy(BestClientId);
				};

				auto TraceLaserForEnemy = [&](vec2 Pos, vec2 Dir, int MaxBounces, float *pOutTravel = nullptr, int *pOutBounces = nullptr) {
					float Energy = Reach;
					float Travel = 0.0f;
					int Bounces = 0;
					while(Energy > 0.0f)
					{
						vec2 Coltile;
						vec2 To = Pos + Dir * Energy;
						const int Res = GameClient()->Collision()->IntersectLineTeleWeapon(Pos, To, &Coltile, &To);
						float HitDist = 0.0f;
						if(SegmentFirstHitIsEnemy(Pos, To, &HitDist, PingCompTicks + Bounces * BounceDelayTicks))
						{
							if(pOutTravel)
								*pOutTravel = Travel + HitDist;
							if(pOutBounces)
								*pOutBounces = Bounces;
							return true;
						}
						if(!Res || Bounces >= MaxBounces)
							break;

						const vec2 From = Pos;
						Pos = To;
						vec2 TempPos = Pos;
						vec2 TempDir = Dir * 4.0f;
						GameClient()->Collision()->MovePoint(&TempPos, &TempDir, 1.0f, nullptr);
						Pos = TempPos;
						if(length(TempDir) > 0.001f)
							Dir = normalize(TempDir);
						const float Distance = distance(From, Pos);
						Travel += Distance;
						Energy -= Distance + BounceCost;
						Bounces++;
					}
					return false;
				};

				int NumSnapActive = NumActiveClients;
				int NumEnemy = 0;
				int NumEnemyTotal = 0;
				int NumTriggerInFov = 0;
				bool TriggerFound = false;
				bool AutoFound = false;
				bool AutoFoundByBounce = false;
				vec2 BestTriggerDir = CurAim;
				vec2 BestAutoDir = CurAim;
				float BestTriggerScore = 1e18f;
				float BestAutoScore = 1e18f;
				float BestTriggerTravel = 0.0f;
				float BestAutoTravel = 0.0f;
				int BestAutoBounces = 0;
				int BestTriggerClientId = -1;
				int BestAutoClientId = -1;
				const float TriggerHalfFov = 36.0f * (pi / 180.0f) * 0.5f;
				const float TriggerCosHalfFov = cosf(TriggerHalfFov);

				auto TryDirectAimPos = [&](int ClientId, const vec2 &AimPos, bool ForTrigger) {
					vec2 To = AimPos - LocalPos;
					const float Dist = length(To);
					if(Dist < 0.001f || Dist > Reach || (ForTrigger && Dist > TriggerReach))
						return;
					To *= 1.0f / Dist;

					const float AimDot = std::clamp(dot(CurAim, To), -1.0f, 1.0f);
					if(ForTrigger && AimDot < TriggerCosHalfFov)
						return;
					const float Angle = acosf(AimDot);
					if(ForTrigger)
						NumTriggerInFov++;

					vec2 HitPos;
					if(GameClient()->Collision()->IntersectLine(LocalPos, AimPos, &HitPos, nullptr) != 0)
						return;

					float HitDist = 0.0f;
					if(!SegmentFirstHitIsEnemy(LocalPos, AimPos, &HitDist))
						return;

					const float Score = Angle * (ForTrigger ? 3000.0f : 2000.0f) + HitDist * 0.20f + Dist * 0.05f;
					if(ForTrigger)
					{
						if(Score < BestTriggerScore)
						{
							BestTriggerScore = Score;
							BestTriggerDir = To;
							BestTriggerTravel = HitDist;
							BestTriggerClientId = ClientId;
							TriggerFound = true;
						}
					}
					else if(Score < BestAutoScore)
					{
						BestAutoScore = Score;
						BestAutoDir = To;
						BestAutoTravel = HitDist;
						BestAutoBounces = 0;
						BestAutoClientId = ClientId;
						AutoFound = true;
					}
				};

				const int MinBounces = FngAimMode == 0 ? (FngFastMode ? 2 : (FngPingMode ? 1 : 0)) :
						       (FngAimMode == 1 ? (FngFastMode ? 3 : (FngPingMode ? 2 : 0)) :
										 (FngFastMode ? 4 : (FngPingMode ? 3 : 0)));
				const int ModeMaxBounces = FngAimMode == 0 ? 2 : (FngAimMode == 1 ? 4 : 8);
				const int MaxBounces = std::clamp(std::max(g_Config.m_ClZzFngAutoLaserMaxBounces, MinBounces), 0, ModeMaxBounces);
				auto ScoreBounceDir = [&](const vec2 &Dir, float &Score, float &Travel, int &Bounces) {
					const float DirLen2 = dot(Dir, Dir);
					if(DirLen2 < 0.000001f)
						return false;
					const vec2 NormalizedDir = Dir * (1.0f / sqrtf(DirLen2));
					Travel = 0.0f;
					Bounces = 0;
					if(!TraceLaserForEnemy(LocalPos, NormalizedDir, MaxBounces, &Travel, &Bounces))
						return false;
					if(Bounces <= 0)
						return false;
					const float Angle = acosf(std::clamp(dot(CurAim, NormalizedDir), -1.0f, 1.0f));
					const float AngleWeight = FngAimMode == 0 ? 3200.0f : (FngAimMode == 1 ? 2600.0f : 2200.0f);
					const float BouncePenalty = FngAimMode == 0 ? 900.0f : (FngAimMode == 1 ? 800.0f : 700.0f);
					const float TravelWeight = FngAimMode == 0 ? 0.25f : (FngAimMode == 1 ? 0.22f : 0.20f);
					Score = Angle * AngleWeight + (float)Bounces * BouncePenalty + Travel * TravelWeight;
					return true;
				};

				auto TryBounceDir = [&](const vec2 &Dir) {
					float Travel = 0.0f;
					int Bounces = 0;
					float Score = 0.0f;
					if(!ScoreBounceDir(Dir, Score, Travel, Bounces))
						return;
					const float PruneMargin = FngAimMode == 0 ? (FngFastMode ? 120.0f : 80.0f) :
									(FngAimMode == 1 ? (FngFastMode ? 260.0f : 150.0f) :
												  (FngFastMode ? 450.0f : 200.0f));
					if(Score >= BestAutoScore + PruneMargin)
						return;

					vec2 RefinedDir = normalize(Dir);
					float RefinedScore = Score;
					float RefinedTravel = Travel;
					int RefinedBounces = Bounces;
					const int RefinePasses = FngAimMode == 0 ? (FngFastMode ? 1 : 0) : (FngAimMode == 1 ? (FngFastMode ? 2 : 1) : (FngFastMode ? 3 : 2));
					float RefineStep = 2.0f * pi / (float)(FngAimMode == 2 && FngFastMode ? 1024 : (FngAimMode == 0 ? 384 : 512));
					for(int Pass = 0; Pass < RefinePasses; Pass++)
					{
						bool Improved = false;
						const float BaseAngle = atan2f(RefinedDir.y, RefinedDir.x);
						for(int Side = -1; Side <= 1; Side += 2)
						{
							const float CandidateAngle = BaseAngle + (float)Side * RefineStep;
							const vec2 CandidateDir = vec2(cosf(CandidateAngle), sinf(CandidateAngle));
							float CandidateScore = 0.0f;
							float CandidateTravel = 0.0f;
							int CandidateBounces = 0;
							if(!ScoreBounceDir(CandidateDir, CandidateScore, CandidateTravel, CandidateBounces))
								continue;
							if(CandidateScore < RefinedScore)
							{
								RefinedScore = CandidateScore;
								RefinedTravel = CandidateTravel;
								RefinedBounces = CandidateBounces;
								RefinedDir = CandidateDir;
								Improved = true;
							}
						}
						if(!Improved)
							RefineStep *= 0.5f;
					}

					if(RefinedScore < BestAutoScore)
					{
						BestAutoScore = RefinedScore;
						BestAutoDir = RefinedDir;
						BestAutoTravel = RefinedTravel;
						BestAutoBounces = RefinedBounces;
						AutoFound = true;
						AutoFoundByBounce = RefinedBounces > 0;
					}
				};

				for(int i = 0; i < MAX_CLIENTS; i++)
				{
					if(i == LocalId)
						continue;
					if(GameClient()->m_Snap.m_apPlayerInfos[i] && IsEnemy(i))
						NumEnemyTotal++;
				}
				for(int ActiveIndex = 0; ActiveIndex < NumActiveClients; ActiveIndex++)
				{
					const int ClientId = aActiveClientIds[ActiveIndex];
					if(!IsEnemy(ClientId))
						continue;
					NumEnemy++;

					vec2 aTargetPositions[12];
					int NumTargetPositions = 0;
					BuildTargetCandidates(ClientId, aTargetPositions, NumTargetPositions, std::size(aTargetPositions), PingCompTicks);
					for(int TargetIndex = 0; TargetIndex < NumTargetPositions; TargetIndex++)
					{
						const vec2 TargetPos = aTargetPositions[TargetIndex];
						TryDirectAimPos(ClientId, TargetPos, true);
						if(UseAutoAim)
							TryDirectAimPos(ClientId, TargetPos, false);
						for(int OffIndex = 0; OffIndex < AimOffsetCount; OffIndex++)
						{
							const vec2 &Off = s_aAimOffsets[OffIndex];
							TryDirectAimPos(ClientId, TargetPos + Off * AimOffsetRadius, true);
							if(UseAutoAim)
								TryDirectAimPos(ClientId, TargetPos + Off * AimOffsetRadius, false);
						}
					}
				}

				if(UseAutoAim && !AutoFound && g_Config.m_ClZzFngAutoLaserBounce)
				{
					if(FngFastMode)
					{
						const float CurAngle = atan2f(CurAim.y, CurAim.x);
						const float FanStep = 2.0f * pi / 192.0f;
						const int FanLimit = FngAimMode == 0 ? 4 : (FngAimMode == 1 ? 8 : 12);
						for(int Fan = -FanLimit; Fan <= FanLimit; Fan++)
						{
							const float Angle = CurAngle + (float)Fan * FanStep;
							TryBounceDir(vec2(cosf(Angle), sinf(Angle)));
						}
					}

					for(int ActiveIndex = 0; ActiveIndex < NumActiveClients; ActiveIndex++)
					{
						const int ClientId = aActiveClientIds[ActiveIndex];
						if(!IsEnemy(ClientId))
							continue;
						vec2 aTargetPositions[12];
						int NumTargetPositions = 0;
						BuildTargetCandidates(ClientId, aTargetPositions, NumTargetPositions, std::size(aTargetPositions), PingCompTicks + BounceDelayTicks);
						for(int TargetIndex = 0; TargetIndex < NumTargetPositions; TargetIndex++)
						{
							const vec2 TargetPos = aTargetPositions[TargetIndex];
							const vec2 ToTarget = TargetPos - LocalPos;
							if(length(ToTarget) > 0.001f)
							{
								const vec2 BaseDir = normalize(ToTarget);
								TryBounceDir(BaseDir);
								for(int OffIndex = 0; OffIndex < AimOffsetCount; OffIndex++)
								{
									const vec2 &Off = s_aAimOffsets[OffIndex];
									const vec2 OffsetDir = TargetPos + Off * AimOffsetRadius - LocalPos;
									if(length(OffsetDir) > 0.001f)
										TryBounceDir(normalize(OffsetDir));
								}
							}
						}
					}

					const int ScanResolution = FngAimMode == 0 ? (FngFastMode ? 256 : (FngPingMode ? 192 : 160)) :
									       (FngAimMode == 1 ? (FngFastMode ? 384 : (FngPingMode ? 288 : 224)) :
												 (FngFastMode ? 512 : (FngPingMode ? 384 : 256)));
					const int BaseRaysPerTick = FngPingMode ? std::max(g_Config.m_ClZzFngAutoLaserRaysPerTick, 48) : g_Config.m_ClZzFngAutoLaserRaysPerTick;
					const int RaysPerTick = FngAimMode == 0 ? (FngFastMode ? 96 : std::clamp(BaseRaysPerTick, 1, 24)) :
							       (FngAimMode == 1 ? (FngFastMode ? 128 : std::clamp(BaseRaysPerTick, 1, 48)) :
										 (FngFastMode ? 192 : std::clamp(BaseRaysPerTick, 1, 64)));
					const float Step = 2.0f * pi / (float)ScanResolution;
					const int StartIndex = s_aFngLaserScanStep[FngDummyIndex] % ScanResolution;
					const float StartAngle = (float)StartIndex * Step;
					const float RayStep = FngFastMode ? 2.0f * pi / (float)RaysPerTick : Step;
					const float StepCos = cosf(RayStep);
					const float StepSin = sinf(RayStep);
					vec2 ScanDir(cosf(StartAngle), sinf(StartAngle));
					for(int r = 0; r < RaysPerTick; r++)
					{
						TryBounceDir(ScanDir);
						ScanDir = vec2(ScanDir.x * StepCos - ScanDir.y * StepSin, ScanDir.x * StepSin + ScanDir.y * StepCos);
					}
					s_aFngLaserScanStep[FngDummyIndex] = (s_aFngLaserScanStep[FngDummyIndex] + (FngFastMode ? 1 : RaysPerTick)) % ScanResolution;
				}

				if(UseAutoAim && AutoFound)
				{
					m_ZzFngDecisionValid = true;
					m_ZzFngDecisionTrigger = false;
					m_ZzFngDecisionBounces = BestAutoBounces;
					m_ZzFngDecisionStart = LocalPos;
					m_ZzFngDecisionDir = BestAutoDir;
					m_ZzFngDecisionTravel = BestAutoTravel;
					m_ZzFngDecisionTick = Client()->GameTick(FngDummyIndex);
				}
				else if(g_Config.m_ClZzFngTriggerLaser && TriggerFound)
				{
					m_ZzFngDecisionValid = true;
					m_ZzFngDecisionTrigger = true;
					m_ZzFngDecisionBounces = 0;
					m_ZzFngDecisionStart = LocalPos;
					m_ZzFngDecisionDir = BestTriggerDir;
					m_ZzFngDecisionTravel = BestTriggerTravel;
					m_ZzFngDecisionTick = Client()->GameTick(FngDummyIndex);
				}

				const bool CanShootNow = LaserSelected && LaserReady && LaserHasAmmo && LaserCanHit && !IsFrozen(LocalId);
				const bool ManualFireHeld = (m_aInputData[FngDummyIndex].m_Fire & 1) != 0;
				const bool ManualAimShot = AimOnFire && LaserSelected && ManualFireHeld && (m_aLastData[FngDummyIndex].m_Fire & 1) == 0;
				if(ManualAimShot && AutoFound && LaserCanHit && !IsFrozen(LocalId))
				{
					m_aInputData[FngDummyIndex].m_TargetX = (int)round_to_int(BestAutoDir.x * 1000.0f);
					m_aInputData[FngDummyIndex].m_TargetY = (int)round_to_int(BestAutoDir.y * 1000.0f);
					if(!m_aInputData[FngDummyIndex].m_TargetX && !m_aInputData[FngDummyIndex].m_TargetY)
						m_aInputData[FngDummyIndex].m_TargetX = 1;
				}
				else if(g_Config.m_ClZzFngAutoLaser && !AimOnFire && AutoFound && LaserCanHit && !IsFrozen(LocalId))
				{
					m_aInputData[FngDummyIndex].m_TargetX = (int)round_to_int(BestAutoDir.x * 1000.0f);
					m_aInputData[FngDummyIndex].m_TargetY = (int)round_to_int(BestAutoDir.y * 1000.0f);
					if(!m_aInputData[FngDummyIndex].m_TargetX && !m_aInputData[FngDummyIndex].m_TargetY)
						m_aInputData[FngDummyIndex].m_TargetX = 1;
					if(!LaserSelected)
					{
						m_aInputData[FngDummyIndex].m_WantedWeapon = WEAPON_LASER + 1;
						SwitchRequested = true;
						s_aFngLaserSwitchKeepTicks[FngDummyIndex] = 4;
					}
					else if(CanShootNow && !ReleasedThisTick && (m_aInputData[FngDummyIndex].m_Fire & 1) == 0)
					{
						m_aInputData[FngDummyIndex].m_Fire = (m_aInputData[FngDummyIndex].m_Fire + 1) & INPUT_STATE_MASK;
						s_aFngLaserReleasePending[FngDummyIndex] = true;
						s_aFngLaserInjectedFire[FngDummyIndex] = m_aInputData[FngDummyIndex].m_Fire;
						InjectedAutoShot = true;
					}
				}
				else if(g_Config.m_ClZzFngTriggerLaser && !AimOnFire && TriggerFound && CanShootNow && !ReleasedThisTick && (m_aInputData[FngDummyIndex].m_Fire & 1) == 0)
				{
					m_aInputData[FngDummyIndex].m_TargetX = (int)round_to_int(BestTriggerDir.x * 1000.0f);
					m_aInputData[FngDummyIndex].m_TargetY = (int)round_to_int(BestTriggerDir.y * 1000.0f);
					if(!m_aInputData[FngDummyIndex].m_TargetX && !m_aInputData[FngDummyIndex].m_TargetY)
						m_aInputData[FngDummyIndex].m_TargetX = 1;
					m_aInputData[FngDummyIndex].m_Fire = (m_aInputData[FngDummyIndex].m_Fire + 1) & INPUT_STATE_MASK;
					s_aFngLaserReleasePending[FngDummyIndex] = true;
					s_aFngLaserInjectedFire[FngDummyIndex] = m_aInputData[FngDummyIndex].m_Fire;
					InjectedTriggerShot = true;
				}
				else if(!SwitchRequested && !LaserSelected)
				{
					s_aFngLaserSwitchKeepTicks[FngDummyIndex] = 0;
				}

				if(g_Config.m_ClZzFngDebug && (ManualAimShot || InjectedAutoShot || InjectedTriggerShot))
				{
					const char *pShotMode = ManualAimShot ? "manual_aim" : (InjectedAutoShot ? "auto" : "trigger");
					const bool UsesAutoTarget = ManualAimShot || InjectedAutoShot;
					const int TargetId = UsesAutoTarget ? BestAutoClientId : BestTriggerClientId;
					char aBuf[384];
					str_format(aBuf, sizeof(aBuf), "Tick=%d Mode=%s TargetId=%d Weapon=%d Ready=%d PingMs=%d PingTicks=%d SnapActive=%d Enemy=%d EnemyTotal=%d Auto=%d Trigger=%d Bounce=%d Aim=(%d,%d) Fire=%d",
						Client()->GameTick(FngDummyIndex), pShotMode, TargetId, pLocalChar->m_Weapon, (int)LaserReady, Client()->GetPredictionTime(), PingCompTicks, NumSnapActive, NumEnemy, NumEnemyTotal, (int)AutoFound, (int)TriggerFound, BestAutoBounces,
						m_aInputData[FngDummyIndex].m_TargetX, m_aInputData[FngDummyIndex].m_TargetY, m_aInputData[FngDummyIndex].m_Fire);
					Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "zz/fng_shot", aBuf);
				}

				if(g_Config.m_ClZzFngDebug)
				{
					static int64_t s_LastDbg = 0;
					const int64_t Now = time_get();
					if(s_LastDbg == 0 || Now - s_LastDbg >= time_freq())
					{
						s_LastDbg = Now;
						char aBuf[512];
						str_format(aBuf, sizeof(aBuf), "LocalId=%d Weapon=%d LaserSel=%d Ready=%d Ammo=%d AmmoOk=%d HitOk=%d LegacyFng2=%d AimOnFire=%d AimMode=%d PingMode=%d FastMode=%d PingTicks=%d BounceDelayTicks=%d TeamPlay=%d LocalTeam=%d SnapActive=%d Enemy=%d EnemyTotal=%d Trigger=%d TriggerFov=%d Auto=%d Bounce=%d SwitchReq=%d KeepTicks=%d ReleasePending=%d Reach=%.1f NewAim=(%d,%d)",
							LocalId, pLocalChar->m_Weapon, (int)LaserSelected, (int)LaserReady, pLocalChar->m_AmmoCount, (int)LaserHasAmmo, (int)LaserCanHit, (int)LegacyFng2Server, (int)AimOnFire, FngAimMode, (int)FngPingMode, (int)FngFastMode, PingCompTicks, BounceDelayTicks, (int)TeamPlay, LocalTeam, NumSnapActive, NumEnemy, NumEnemyTotal, (int)TriggerFound, NumTriggerInFov, (int)AutoFound, (int)AutoFoundByBounce, (int)SwitchRequested, s_aFngLaserSwitchKeepTicks[FngDummyIndex], (int)s_aFngLaserReleasePending[FngDummyIndex], Reach, m_aInputData[FngDummyIndex].m_TargetX, m_aInputData[FngDummyIndex].m_TargetY);
						Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "zz/fng_laser", aBuf);
					}
				}
			}
		}
		else
		{
			s_aFngLaserReleasePending[FngDummyIndex] = false;
			s_aFngLaserInjectedFire[FngDummyIndex] = -1;
			s_aFngLaserSwitchKeepTicks[FngDummyIndex] = 0;
		}


		// Rapid fire implementation
		if(g_Config.m_ClZzRapidFire)
		{
			const int64_t Now = time_get();
			const int64_t RapidFireDelayTicks = (int64_t)g_Config.m_ClZzRapidFireDelay * time_freq() / 1000;
			
			// Check if fire button is pressed
			const bool FirePressed = (m_aInputData[g_Config.m_ClDummy].m_Fire & 1) != 0;
			
			// Detect press/release
			if(FirePressed && !m_aRapidFireWasPressed[g_Config.m_ClDummy])
			{
				// Fire button just pressed
				m_aRapidFireActive[g_Config.m_ClDummy] = true;
				m_aRapidFireLastTick[g_Config.m_ClDummy] = Now;
				m_aRapidFireWasPressed[g_Config.m_ClDummy] = true;
			}
			else if(!FirePressed && m_aRapidFireWasPressed[g_Config.m_ClDummy])
			{
				// Fire button just released
				m_aRapidFireActive[g_Config.m_ClDummy] = false;
				m_aRapidFireWasPressed[g_Config.m_ClDummy] = false;
			}
			
			// Handle rapid fire
			if(m_aRapidFireActive[g_Config.m_ClDummy])
			{
				if(Now - m_aRapidFireLastTick[g_Config.m_ClDummy] >= RapidFireDelayTicks)
				{
					// Fire!
					m_aInputData[g_Config.m_ClDummy].m_Fire = (m_aInputData[g_Config.m_ClDummy].m_Fire + 1) & INPUT_STATE_MASK;
					m_aRapidFireLastTick[g_Config.m_ClDummy] = Now;
				}
			}
		}
		
		
		// dummy copy moves
		if(g_Config.m_ClDummyCopyMoves)
		{
			CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;

			// Don't copy any input to dummy when spectating others
			if(!GameClient()->m_Snap.m_SpecInfo.m_Active || GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
			{
				pDummyInput->m_Direction = m_aInputData[g_Config.m_ClDummy].m_Direction;
				pDummyInput->m_Hook = m_aInputData[g_Config.m_ClDummy].m_Hook;
				pDummyInput->m_Jump = m_aInputData[g_Config.m_ClDummy].m_Jump;
				pDummyInput->m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;
				pDummyInput->m_TargetX = m_aInputData[g_Config.m_ClDummy].m_TargetX;
				pDummyInput->m_TargetY = m_aInputData[g_Config.m_ClDummy].m_TargetY;
				pDummyInput->m_WantedWeapon = m_aInputData[g_Config.m_ClDummy].m_WantedWeapon;

				if(!g_Config.m_ClDummyControl)
					pDummyInput->m_Fire += m_aInputData[g_Config.m_ClDummy].m_Fire - m_aLastData[g_Config.m_ClDummy].m_Fire;

				pDummyInput->m_NextWeapon += m_aInputData[g_Config.m_ClDummy].m_NextWeapon - m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
				pDummyInput->m_PrevWeapon += m_aInputData[g_Config.m_ClDummy].m_PrevWeapon - m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
			}

			m_aInputData[Client()->DummyPair()] = *pDummyInput;
		}

		if(g_Config.m_ClDummyControl)
		{
			CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;
			pDummyInput->m_Jump = g_Config.m_ClDummyJump;

			if(g_Config.m_ClDummyFire)
				pDummyInput->m_Fire = g_Config.m_ClDummyFire;
			else if((pDummyInput->m_Fire & 1) != 0)
				pDummyInput->m_Fire++;

			pDummyInput->m_Hook = g_Config.m_ClDummyHook;
		}

		// stress testing
		if(g_Config.m_DbgStress)
		{
			float t = Client()->LocalTime();
			mem_zero(&m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

			m_aInputData[g_Config.m_ClDummy].m_Direction = ((int)t / 2) & 1;
			m_aInputData[g_Config.m_ClDummy].m_Jump = ((int)t);
			m_aInputData[g_Config.m_ClDummy].m_Fire = ((int)(t * 10));
			m_aInputData[g_Config.m_ClDummy].m_Hook = ((int)(t * 2)) & 1;
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = ((int)t) % NUM_WEAPONS;
			m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)(std::sin(t * 3) * 100.0f);
			m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)(std::cos(t * 3) * 100.0f);
		}

		// Avoid and Laser Unfreeze are evaluated in CGameClient after this
		// base input has been assembled. Force a tick-rate packet while either is
		// enabled; otherwise SnapInput may return 0 before the predictive code gets
		// a chance to run, which made both features appear inactive while idle.
		if((g_Config.m_ClZzAvoidEnabled || g_Config.m_ClZzAutoUnfreeze) &&
			GameClient()->m_Snap.m_pLocalCharacter)
		{
			const int TickSpeed = maximum(1, Client()->GameTickSpeed());
			Send = Send || time_get() > m_LastSendTime + time_freq() / TickSpeed;
		}

		// check if we need to send input
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Direction != m_aLastData[g_Config.m_ClDummy].m_Direction;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Jump != m_aLastData[g_Config.m_ClDummy].m_Jump;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Fire != m_aLastData[g_Config.m_ClDummy].m_Fire;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Hook != m_aLastData[g_Config.m_ClDummy].m_Hook;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_WantedWeapon != m_aLastData[g_Config.m_ClDummy].m_WantedWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_NextWeapon != m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_PrevWeapon != m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
		Send = Send || time_get() > m_LastSendTime + time_freq() / 25; // send at least 25 Hz

		// KernelNet: while hammer assist is actively trying to hit (hammer + fire held), send at tickrate.
		if(g_Config.m_ClZzHammerAssistEnabled && GameClient()->m_Snap.m_pLocalCharacter &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_HAMMER && (m_aInputData[g_Config.m_ClDummy].m_Fire & 1))
		{
			const int TickSpeed = maximum(1, Client()->GameTickSpeed());
			Send = Send || time_get() > m_LastSendTime + time_freq() / TickSpeed;
		}
		// Auto ALED has exactly one server tick in which a tee is naturally
		// unfrozen behind the freeze wall. Evaluate/send at server tickrate even
		// while the user is idle, otherwise the normal 25 Hz throttle can skip it.
		if(g_Config.m_ClZzAutoAled && GameClient()->m_Snap.m_pLocalCharacter)
		{
			const int TickSpeed = maximum(1, Client()->GameTickSpeed());
			Send = Send || time_get() > m_LastSendTime + time_freq() / TickSpeed;
		}
		// Tile Aim updates the packet-only hook direction while the local tee is
		// moving and may need a one-tick release/repress after a miss.
		if(m_aSaveInBlock[g_Config.m_ClDummy] && GameClient()->m_Snap.m_pLocalCharacter)
		{
			const int TickSpeed = maximum(1, Client()->GameTickSpeed());
			Send = Send || time_get() > m_LastSendTime + time_freq() / TickSpeed;
		}
		if(g_Config.m_ClZzBalanceBot && GameClient()->m_Snap.m_pLocalCharacter)
		{
			const int TickSpeed = maximum(1, Client()->GameTickSpeed());
			Send = Send || time_get() > m_LastSendTime + time_freq() / TickSpeed;
		}
		if(g_Config.m_ClZzFlyRide && GameClient()->FlyRideActive())
		{
			const int TickSpeed = maximum(1, Client()->GameTickSpeed());
			Send = Send || time_get() > m_LastSendTime + time_freq() / TickSpeed;
		}
		Send = Send || (GameClient()->m_Snap.m_pLocalCharacter && GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_NINJA && (m_aInputData[g_Config.m_ClDummy].m_Direction || m_aInputData[g_Config.m_ClDummy].m_Jump || m_aInputData[g_Config.m_ClDummy].m_Hook));
	}

	const CNetObj_PlayerInput OldLastData = m_aLastData[g_Config.m_ClDummy];

	// copy and return size
	m_aLastData[g_Config.m_ClDummy] = m_aInputData[g_Config.m_ClDummy];

	if(!Send)
		return 0;

	// Kinetix Laser Unfreeze silent aim has final aim priority. It is applied
	// after the visible mouse position was converted to input, so the server
	// receives the bounce direction while the user's crosshair stays untouched.
	const bool LaserUnfreezeAimActive = m_LaserUnfreezeAimActive;
	if(LaserUnfreezeAimActive)
	{
		m_aInputData[g_Config.m_ClDummy].m_TargetX =
			(int)m_LaserUnfreezeAimOffset.x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY =
			(int)m_LaserUnfreezeAimOffset.y;
		if(m_aInputData[g_Config.m_ClDummy].m_TargetX == 0 &&
			m_aInputData[g_Config.m_ClDummy].m_TargetY == 0)
			m_aInputData[g_Config.m_ClDummy].m_TargetY = 1;
		m_LaserUnfreezeAimActive = false;
	}

	if(g_Config.m_ClZzFngDebug)
	{
		static int64_t s_LastSendDbg = 0;
		const int64_t Now = time_get();
		if(s_LastSendDbg == 0 || Now - s_LastSendDbg >= time_freq())
		{
			s_LastSendDbg = Now;
			const CNetObj_PlayerInput &Cur = m_aInputData[g_Config.m_ClDummy];
			int Changed = 0;
			if(Cur.m_Direction != OldLastData.m_Direction) Changed |= 1 << 0;
			if(Cur.m_Jump != OldLastData.m_Jump) Changed |= 1 << 1;
			if(Cur.m_Fire != OldLastData.m_Fire) Changed |= 1 << 2;
			if(Cur.m_Hook != OldLastData.m_Hook) Changed |= 1 << 3;
			if(Cur.m_WantedWeapon != OldLastData.m_WantedWeapon) Changed |= 1 << 4;
			if(Cur.m_NextWeapon != OldLastData.m_NextWeapon) Changed |= 1 << 5;
			if(Cur.m_PrevWeapon != OldLastData.m_PrevWeapon) Changed |= 1 << 6;
			if(Cur.m_TargetX != OldLastData.m_TargetX || Cur.m_TargetY != OldLastData.m_TargetY) Changed |= 1 << 7;

			int Reason = 0;
			if(Changed != 0) Reason |= 1 << 0;
			if(time_get() > m_LastSendTime + time_freq() / 25) Reason |= 1 << 1;
			if(g_Config.m_ClZzHammerAssistEnabled && GameClient()->m_Snap.m_pLocalCharacter &&
				GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_HAMMER && (Cur.m_Fire & 1))
				Reason |= 1 << 2;
			if(GameClient()->m_Snap.m_pLocalCharacter && GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_NINJA && (Cur.m_Direction || Cur.m_Jump || Cur.m_Hook))
				Reason |= 1 << 3;

			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "Tick=%d Size=%d ReasonMask=0x%x ChangedMask=0x%x Fire=%d Hook=%d Dir=%d Jump=%d Aim=(%d,%d)", Client()->GameTick(g_Config.m_ClDummy), (int)sizeof(m_aInputData[0]), Reason, Changed, Cur.m_Fire, Cur.m_Hook, Cur.m_Direction, Cur.m_Jump, Cur.m_TargetX, Cur.m_TargetY);
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "zz/fng_send", aBuf);
		}
	}

	m_LastSendTime = time_get();
	mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));
	return sizeof(m_aInputData[0]);
}

void CControls::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	if(g_Config.m_ClAutoswitchWeaponsOutOfAmmo && !GameClient()->m_GameInfo.m_UnlimitedAmmo && GameClient()->m_Snap.m_pLocalCharacter)
	{
		// Keep track of ammo count, we know weapon ammo only when we switch to that weapon, this is tracked on server and protocol does not track that
		m_aAmmoCount[maximum(0, GameClient()->m_Snap.m_pLocalCharacter->m_Weapon % NUM_WEAPONS)] = GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount;
		// Autoswitch weapon if we're out of ammo
		if(m_aInputData[g_Config.m_ClDummy].m_Fire % 2 != 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount == 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_HAMMER &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_NINJA)
		{
			int Weapon;
			for(Weapon = WEAPON_LASER; Weapon > WEAPON_GUN; Weapon--)
			{
				if(Weapon == GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
					continue;
				if(m_aAmmoCount[Weapon] > 0)
					break;
			}
			if(Weapon != GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
				m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = Weapon + 1;
		}
	}

	// update target pos
	if(GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		// make sure to compensate for smooth dyncam to ensure the cursor stays still in world space if zoomed
		vec2 DyncamOffsetDelta = GameClient()->m_Camera.m_DyncamTargetCameraOffset - GameClient()->m_Camera.m_aDyncamCurrentCameraOffset[g_Config.m_ClDummy];
		float Zoom = GameClient()->m_Camera.m_Zoom;
		m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_LocalCharacterPos + m_aMousePos[g_Config.m_ClDummy] - DyncamOffsetDelta + DyncamOffsetDelta / Zoom;
	}
	else if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_UsePosition)
	{
		m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_Snap.m_SpecInfo.m_Position + m_aMousePos[g_Config.m_ClDummy];
	}
	else
	{
		m_aTargetPos[g_Config.m_ClDummy] = m_aMousePos[g_Config.m_ClDummy];
	}
}

bool CControls::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	if(GameClient()->m_Snap.m_pGameInfoObj && (GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED))
		return false;

	if(CursorType == IInput::CURSOR_JOYSTICK && g_Config.m_InpControllerAbsolute && GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		vec2 AbsoluteDirection;
		if(Input()->GetActiveJoystick()->Absolute(&AbsoluteDirection.x, &AbsoluteDirection.y))
		{
			m_aMousePos[g_Config.m_ClDummy] = AbsoluteDirection * GetMaxMouseDistance();
			GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::ABSOLUTE;
		}
		return true;
	}

	float Factor = 1.0f;
	if(g_Config.m_ClDyncam && g_Config.m_ClDyncamMousesens)
	{
		Factor = g_Config.m_ClDyncamMousesens / 100.0f;
	}
	else
	{
		switch(CursorType)
		{
		case IInput::CURSOR_MOUSE:
			Factor = g_Config.m_InpMousesens / 100.0f;
			break;
		case IInput::CURSOR_JOYSTICK:
			Factor = g_Config.m_InpControllerSens / 100.0f;
			break;
		default:
			dbg_assert_failed("CControls::OnCursorMove CursorType %d", (int)CursorType);
		}
	}

	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
		Factor *= GameClient()->m_Camera.m_Zoom;

	m_aMousePos[g_Config.m_ClDummy] += vec2(x, y) * Factor;
	GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::RELATIVE;
	ClampMousePos();
	return true;
}

void CControls::ClampMousePos()
{
	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
	{
		m_aMousePos[g_Config.m_ClDummy].x = std::clamp(m_aMousePos[g_Config.m_ClDummy].x, -201.0f * 32, (Collision()->GetWidth() + 201.0f) * 32.0f);
		m_aMousePos[g_Config.m_ClDummy].y = std::clamp(m_aMousePos[g_Config.m_ClDummy].y, -201.0f * 32, (Collision()->GetHeight() + 201.0f) * 32.0f);
	}
	else
	{
		const float MouseMin = GetMinMouseDistance();
		const bool ChatActive = GameClient()->m_Chat.IsActive();
		const float MouseMax = GetMaxMouseDistance() * (ChatActive ? 2.5f : 1.0f);

		float MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance < 0.001f)
		{
			m_aMousePos[g_Config.m_ClDummy].x = 0.001f;
			m_aMousePos[g_Config.m_ClDummy].y = 0;
			MouseDistance = 0.001f;
		}
		if(MouseDistance < MouseMin)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMin;
		MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance > MouseMax)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMax;

		if(g_Config.m_TcLimitMouseToScreen)
		{
			float Width, Height;
			Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), 1.0f, &Width, &Height);
			Height /= 2.0f;
			Width /= 2.0f;
			if(ChatActive)
			{
				Height *= 2.5f;
				Width *= 2.5f;
			}
			if(g_Config.m_TcLimitMouseToScreen == 2)
				Width = Height;
			m_aMousePos[g_Config.m_ClDummy].y = std::clamp(m_aMousePos[g_Config.m_ClDummy].y, -Height, Height);
			m_aMousePos[g_Config.m_ClDummy].x = std::clamp(m_aMousePos[g_Config.m_ClDummy].x, -Width, Width);
		}
	}
}

float CControls::GetMinMouseDistance() const
{
	return g_Config.m_ClDyncam ? g_Config.m_ClDyncamMinDistance : g_Config.m_ClMouseMinDistance;
}

float CControls::GetMaxMouseDistance() const
{
	float CameraMaxDistance = 200.0f;
	float FollowFactor = (g_Config.m_ClDyncam ? g_Config.m_ClDyncamFollowFactor : g_Config.m_ClMouseFollowfactor) / 100.0f;
	float DeadZone = g_Config.m_ClDyncam ? g_Config.m_ClDyncamDeadzone : g_Config.m_ClMouseDeadzone;
	float MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
	return minimum((FollowFactor != 0 ? CameraMaxDistance / FollowFactor + DeadZone : MaxDistance), MaxDistance);
}

bool CControls::CheckNewInput()
{
	const int ActiveConn = std::clamp(g_Config.m_ClDummy, 0, NUM_DUMMIES - 1);
	const int PairConn = std::clamp(Client()->DummyPair(), 0, NUM_DUMMIES - 1);
	const bool UiOwnsInput = GameClient()->m_Chat.IsActive() ||
		GameClient()->m_Menus.IsActive() || g_Config.m_ClZzClickGui != 0;
	if(UiOwnsInput || m_aUiInputFrozen[ActiveConn])
	{
		m_FastInputHookAction = false;
		m_FastInputFireAction = false;
		return false;
	}

	bool NewInput[NUM_DUMMIES] = {};
	for(int Dummy = 0; Dummy < NUM_DUMMIES; Dummy++)
	{
		CNetObj_PlayerInput TestInput = m_aInputData[Dummy];
		if(Dummy == ActiveConn)
		{
			TestInput.m_Direction = 0;
			if(m_aInputDirectionLeft[Dummy] && !m_aInputDirectionRight[Dummy])
				TestInput.m_Direction = -1;
			if(!m_aInputDirectionLeft[Dummy] && m_aInputDirectionRight[Dummy])
				TestInput.m_Direction = 1;
		}

		if(m_aFastInput[Dummy].m_Direction != TestInput.m_Direction)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Hook != TestInput.m_Hook)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Fire != TestInput.m_Fire)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Jump != TestInput.m_Jump)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_NextWeapon != TestInput.m_NextWeapon)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_PrevWeapon != TestInput.m_PrevWeapon)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_WantedWeapon != TestInput.m_WantedWeapon)
			NewInput[Dummy] = true;

		bool SetMousePos = false;
		// We need to be careful about how we manage the mouse position to avoid mispredicted hooks and fires
		// on the first tick that they activate before we know what mouse position we actually sent to the server
		if(Dummy == ActiveConn)
		{
			if(m_aFastInput[Dummy].m_Hook == 0 && TestInput.m_Hook == 1)
			{
				m_FastInputHookAction = true;
				SetMousePos = true;
			}
			if(m_aFastInput[Dummy].m_Fire != TestInput.m_Fire && TestInput.m_Fire % 2 == 1)
			{
				m_FastInputFireAction = true;
				SetMousePos = true;
			}
			if(!m_FastInputHookAction && !m_FastInputFireAction)
			{
				SetMousePos = true;
			}
		}

		if(SetMousePos)
		{
			TestInput.m_TargetX = (int)m_aMousePos[Dummy].x;
			TestInput.m_TargetY = (int)m_aMousePos[Dummy].y;
		}
		else
		{
			TestInput.m_TargetX = m_aFastInput[Dummy].m_TargetX;
			TestInput.m_TargetY = m_aFastInput[Dummy].m_TargetY;
		}

		m_aFastInput[Dummy] = TestInput;
	}

	return NewInput[ActiveConn] || NewInput[PairConn];
}
