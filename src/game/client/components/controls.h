/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_CONTROLS_H
#define GAME_CLIENT_COMPONENTS_CONTROLS_H

#include <base/vmath.h>

#include <engine/client.h>
#include <engine/console.h>

#include <generated/protocol.h>

#include <game/client/component.h>

class CControls : public CComponent
{
public:
	float GetMinMouseDistance() const;
	float GetMaxMouseDistance() const;

	enum class EMouseInputType
	{
		ABSOLUTE,
		RELATIVE,
		AUTOMATED,
	};

	vec2 m_aMousePos[NUM_DUMMIES];
	vec2 m_aMousePosOnAction[NUM_DUMMIES];
	vec2 m_aTargetPos[NUM_DUMMIES];

	EMouseInputType m_aMouseInputType[NUM_DUMMIES];

	int m_aAmmoCount[NUM_WEAPONS];

	int64_t m_LastSendTime;
	CNetObj_PlayerInput m_aInputData[NUM_DUMMIES];
	CNetObj_PlayerInput m_aLastData[NUM_DUMMIES];
	int m_aInputDirectionLeft[NUM_DUMMIES];
	int m_aInputDirectionRight[NUM_DUMMIES];
	int m_aShowHookColl[NUM_DUMMIES];
	int m_aSaveInBlock[NUM_DUMMIES];

	// Kinetix Laser Unfreeze silent input channel. UpdateLaserUnfreeze sets
	// this without moving m_aMousePos; SnapInput consumes it immediately before
	// the packet is copied to the network buffer.
	bool m_LaserUnfreezeAimActive = false;
	vec2 m_LaserUnfreezeAimOffset = vec2(0.0f, 0.0f);

	bool m_ZzFngDecisionValid = false;
	bool m_ZzFngDecisionTrigger = false;
	int m_ZzFngDecisionBounces = 0;
	vec2 m_ZzFngDecisionStart = vec2(0.0f, 0.0f);
	vec2 m_ZzFngDecisionDir = vec2(1.0f, 0.0f);
	float m_ZzFngDecisionTravel = 0.0f;
	int m_ZzFngDecisionTick = -1;

	// TClient
	CNetObj_PlayerInput m_aFastInput[NUM_DUMMIES];
	// Gameplay input was released for an active chat/menu. Keep fast prediction
	// disabled until the UI is closed, so it cannot reuse a pre-UI action.
	bool m_aUiInputFrozen[NUM_DUMMIES] = {};
	bool m_FastInputHookAction = false;
	bool m_FastInputFireAction = false;

	// Rapid fire
	int64_t m_aRapidFireLastTick[NUM_DUMMIES];
	bool m_aRapidFireActive[NUM_DUMMIES];
	bool m_aRapidFireWasPressed[NUM_DUMMIES];

	CControls();
	int Sizeof() const override { return sizeof(*this); }

	void OnReset() override;
	void OnRender() override;
	void OnMessage(int MsgType, void *pRawMsg) override;
	bool OnCursorMove(float x, float y, IInput::ECursorType CursorType) override;
	void OnConsoleInit() override;
	virtual void OnPlayerDeath();

	int SnapInput(int *pData);
	void ClampMousePos();
	void ResetInput(int Dummy);
	bool CheckNewInput();

private:
	static void ConKeyInputState(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputSet(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData);
};
#endif
