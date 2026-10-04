#include "zz_clickgui.h"

#include <base/color.h>
#include <base/str.h>

#include <engine/client.h>
#include <engine/font_icons.h>
#include <engine/graphics.h>
#include <engine/keys.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <game/client/components/binds.h>
#include <game/client/components/console.h>
#include <game/client/gameclient.h>
#include <game/client/zz_theme.h>
#include <game/client/zz_visual_defaults.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace
{
	enum class ERowType
	{
		ACTION,
		TOGGLE,
		SLIDER,
		MODE,
		KEYBIND,
	};

	struct SRow
	{
		ERowType m_Type;
		const char *m_pName;
		int CConfig::*m_pValue;
		int m_Min;
		int m_Max;
		int m_Step;
		const char *const *m_ppOptions;
		int m_NumOptions;
		const char *m_pBindCommand = nullptr;
		int CConfig::*m_pModifier = nullptr;
	};

	constexpr float RowHeight(ERowType Type)
	{
		return Type == ERowType::SLIDER ? 45.0f : 29.0f;
	}

	static const char *const s_apAimModes[] = {"Light", "Medium", "Strong"};
	static const char *const s_apThemes[] = {"Custom", "Violet", "Ruby",
		"Emerald", "Cyber", "Mono"};
	static const char *const s_apAspectRatios[] = {"Native", "4:3", "5:4",
		"16:10", "16:9", "21:9"};

	static SRow s_aAimRows[] = {
		{ERowType::TOGGLE, "Aimbot", &CConfig::m_ClZzAimbotEnabled, 0, 1, 1,
			nullptr, 0},
		{ERowType::SLIDER, "Aim FOV", &CConfig::m_ClZzAimbotFov, 1, 180, 1, nullptr,
			0},
		{ERowType::TOGGLE, "Prefer frozen", &CConfig::m_ClZzAimbotPreferFrozen, 0,
			1, 1, nullptr, 0},
		{ERowType::TOGGLE, "Ignore friends", &CConfig::m_ClZzAimbotIgnoreFriends, 0,
			1, 1, nullptr, 0},
		{ERowType::TOGGLE, "Silent aim", &CConfig::m_ClZzAimbotSilent, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Show FOV", &CConfig::m_ClZzAimbotShowFov, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Hammer assist", &CConfig::m_ClZzHammerAssistEnabled, 0,
			1, 1, nullptr, 0},
		{ERowType::TOGGLE, "Auto aled", &CConfig::m_ClZzAutoAled, 0, 1, 1, nullptr,
			0},
		{ERowType::TOGGLE, "Hook aim (blocks)", &CConfig::m_ClKnHookBlocks, 0, 1,
			1, nullptr, 0},
		{ERowType::TOGGLE, "Hook tile silent", &CConfig::m_ClZzSaveInBlockSilent,
			0, 1, 1, nullptr, 0},
	};

	static SRow s_aFngRows[] = {
		{ERowType::TOGGLE, "Trigger laser", &CConfig::m_ClZzFngTriggerLaser, 0, 1,
			1, nullptr, 0},
		{ERowType::SLIDER, "Trigger distance", &CConfig::m_ClZzFngTriggerDistance,
			1, 26, 1, nullptr, 0},
		{ERowType::TOGGLE, "Decision indicator", &CConfig::m_ClZzFngShowDecision, 0,
			1, 1, nullptr, 0},
		{ERowType::TOGGLE, "Auto laser 360", &CConfig::m_ClZzFngAutoLaser, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Aim on LMB", &CConfig::m_ClZzFngAimOnFire, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Ricochets", &CConfig::m_ClZzFngAutoLaserBounce, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Ping mode", &CConfig::m_ClZzFngPingMode, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Fast mode", &CConfig::m_ClZzFngFastMode, 0, 1, 1,
			nullptr, 0},
		{ERowType::MODE, "Aim mode", &CConfig::m_ClZzFngAimMode, 0, 2, 1,
			s_apAimModes, 3},
		{ERowType::SLIDER, "Rays per tick", &CConfig::m_ClZzFngAutoLaserRaysPerTick,
			1, 64, 1, nullptr, 0},
		{ERowType::SLIDER, "Max bounces", &CConfig::m_ClZzFngAutoLaserMaxBounces, 0,
			8, 1, nullptr, 0},
		{ERowType::TOGGLE, "Debug", &CConfig::m_ClZzFngDebug, 0, 1, 1, nullptr, 0},
	};

	static SRow s_aMovementRows[] = {
		{ERowType::TOGGLE, "Avoid", &CConfig::m_ClZzAvoidEnabled, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Freeze", &CConfig::m_ClZzAvoidFreeze, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Teleport", &CConfig::m_ClZzAvoidTele, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Death", &CConfig::m_ClZzAvoidKill, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Direction", &CConfig::m_ClZzAvoidDirection, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Jump", &CConfig::m_ClZzAvoidJump, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Hook", &CConfig::m_ClZzAvoidHookAssist, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Aim", &CConfig::m_ClZzAvoidAim, 0, 1, 1, nullptr, 0},
		{ERowType::SLIDER, "Aim FOV", &CConfig::m_ClZzAvoidFov, 5, 360, 5,
			nullptr, 0},
		{ERowType::SLIDER, "Aim angles", &CConfig::m_ClZzAvoidAngles, 1, 16, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Silent", &CConfig::m_ClZzAvoidSilent, 0, 1, 1,
			nullptr, 0},
		{ERowType::SLIDER, "Simulation ticks", &CConfig::m_ClZzAvoidTicks, 1, 20,
			1, nullptr, 0},
		{ERowType::SLIDER, "Trigger ticks", &CConfig::m_ClZzAvoidTriggerTicks, 1,
			20, 1, nullptr, 0},
		{ERowType::TOGGLE, "Balance bot", &CConfig::m_ClZzBalanceBot, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Fly", &CConfig::m_ClZzFlyHelperEnabled, 0, 1, 1,
			nullptr, 0},
		{ERowType::SLIDER, "Hit range x0.1", &CConfig::m_ClZzFlyHelperHitTiles, 1,
			80, 1, nullptr, 0},
		{ERowType::SLIDER, "Hit delay ms", &CConfig::m_ClZzFlyHelperHitMs, 0, 1000,
			10, nullptr, 0},
		{ERowType::TOGGLE, "Relaxed server", &CConfig::m_ClZzFlyHelperRelaxed, 0, 1,
			1, nullptr, 0},
		{ERowType::TOGGLE, "Hook drive", &CConfig::m_ClZzFlyRide, 0, 1, 1,
			nullptr, 0},
	};

	static SRow s_aUtilityRows[] = {
		{ERowType::ACTION, "Use vanilla DDNet visuals", nullptr, 0, 0, 0, nullptr,
			0},
		{ERowType::MODE, "Theme", &CConfig::m_ClZzTheme, 0, 5, 1, s_apThemes, 6},
		{ERowType::MODE, "Aspect ratio", &CConfig::m_ClZzAspectRatio, 0, 5, 1,
			s_apAspectRatios, 6},
		{ERowType::TOGGLE, "Click GUI animations",
			&CConfig::m_ClZzClickGuiAnimations, 0, 1, 1, nullptr, 0},
		{ERowType::SLIDER, "Animation speed", &CConfig::m_ClZzAnimationSpeed, 25,
			200, 5, nullptr, 0},
		{ERowType::TOGGLE, "Visual effects", &CConfig::m_ClZzVisualEffects, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Shader bloom", &CConfig::m_ClZzPostProcessing, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Ambient sweep", &CConfig::m_ClZzVisualAmbientSweep, 0,
			1, 1, nullptr, 0},
		{ERowType::TOGGLE, "Focus brackets",
			&CConfig::m_ClZzVisualFocusBrackets, 0, 1, 1, nullptr, 0},
		{ERowType::SLIDER, "Effects intensity",
			&CConfig::m_ClZzVisualEffectsIntensity, 0, 100, 5, nullptr, 0},
		{ERowType::TOGGLE, "Chat bubbles", &CConfig::m_ClZzChatBubbles, 0, 1, 1,
			nullptr, 0},
		{ERowType::SLIDER, "Bubble duration", &CConfig::m_ClZzChatBubbleDuration, 2,
			10, 1, nullptr, 0},
	};

	static SRow s_aKernelNetRows[] = {
		{ERowType::TOGGLE, "Laser unfreeze", &CConfig::m_ClZzAutoUnfreeze, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Auto laser", &CConfig::m_ClZzAutoUnfreezeAutoLaser, 0,
			1, 1, nullptr, 0},
		{ERowType::SLIDER, "Laser FOV", &CConfig::m_ClZzAutoUnfreezeFov, 5, 360,
			5, nullptr, 0},
		{ERowType::SLIDER, "Laser angles", &CConfig::m_ClZzAutoUnfreezeAngles, 1,
			144, 1, nullptr, 0},
		{ERowType::SLIDER, "Laser ticks", &CConfig::m_ClZzAutoUnfreezeTicks, 1, 50,
			1, nullptr, 0},
		{ERowType::SLIDER, "Laser trigger ticks",
			&CConfig::m_ClZzAutoUnfreezeTriggerTicks, 1, 5, 1, nullptr, 0},
		{ERowType::TOGGLE, "Laser silent", &CConfig::m_ClZzAutoUnfreezeSilent, 0,
			1, 1, nullptr, 0},
		{ERowType::TOGGLE, "Show laser attempt",
			&CConfig::m_ClZzAutoUnfreezeShowAttempt, 0, 1, 1, nullptr, 0},
		{ERowType::TOGGLE, "Unfreeze debug recording",
			&CConfig::m_ClZzAutoUnfreezeDebugRecord, 0, 1, 1, nullptr, 0},
		{ERowType::TOGGLE, "Full freeze session recording",
			&CConfig::m_ClZzFreezeFullDebugRecord, 0, 1, 1, nullptr, 0},
		{ERowType::TOGGLE, "Auto shotgun", &CConfig::m_ClZzAutoShotgun, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Hammer gun", &CConfig::m_ClZzHammerGun, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Safe hammer gun",
			&CConfig::m_ClZzHammerGunAutoDisableExtraWeapons, 0, 1, 1, nullptr, 0},
		{ERowType::TOGGLE, "Force zoom", &CConfig::m_ClZzForceZoom, 0, 1, 1,
			nullptr, 0},
		{ERowType::SLIDER, "Fake ping", &CConfig::m_ClZzFakePing, 0, 999, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Companion", &CConfig::m_ClZzCompanionEnabled, 0, 1, 1,
			nullptr, 0},
		{ERowType::SLIDER, "Follow tiles", &CConfig::m_ClZzCompanionFollowTiles, 0,
			30, 1, nullptr, 0},
		{ERowType::TOGGLE, "Auto grenade save", &CConfig::m_ClKnGrenadeSave,
			0, 1, 1, nullptr, 0},
		{ERowType::TOGGLE, "Emoji chat button", &CConfig::m_ClKnEmojiButton,
			0, 1, 1, nullptr, 0},
		{ERowType::TOGGLE, "Observers", &CConfig::m_ClKnWatcherList,
			0, 1, 1, nullptr, 0},
		{ERowType::TOGGLE, "Steal nearest identity", &CConfig::m_ClKnIdentitySteal,
			0, 1, 1, nullptr, 0},
		{ERowType::TOGGLE, "Weapon prediction", &CConfig::m_ClKnWeaponPrediction,
			0, 1, 1, nullptr, 0},
	};

	static SRow s_aBindRows[] = {
		{ERowType::KEYBIND, "ClickGUI", &CConfig::m_ClZzClickGuiKey, 0, 512, 1,
			nullptr, 0, nullptr, &CConfig::m_ClZzClickGuiMod},
		{ERowType::KEYBIND, "Aimbot", &CConfig::m_ClZzAimbotKey, 0, 512, 1,
			nullptr, 0, "toggle cl_zz_aimbot_enabled 0 1", &CConfig::m_ClZzAimbotMod},
		{ERowType::KEYBIND, "Hook aim", &CConfig::m_ClKnHookBlocksKey, 0, 512, 1,
			nullptr, 0, "toggle cl_kn_01 0 1", &CConfig::m_ClKnHookBlocksMod},
		{ERowType::KEYBIND, "Hook tile", &CConfig::m_ClZzSaveInBlockKey, 0, 512, 1,
			nullptr, 0, "+zz_tile_aimbot", &CConfig::m_ClZzSaveInBlockMod},
		{ERowType::KEYBIND, "Hammer assist", &CConfig::m_ClZzHammerAssistKey, 0, 512, 1,
			nullptr, 0, "toggle cl_zz_hammer_assist_enabled 0 1", &CConfig::m_ClZzHammerAssistMod},
		{ERowType::KEYBIND, "Auto ALED", &CConfig::m_ClZzAutoAledKey, 0, 512, 1,
			nullptr, 0, "toggle cl_zz_auto_aled 0 1", &CConfig::m_ClZzAutoAledMod},
		{ERowType::KEYBIND, "FNG auto laser", &CConfig::m_ClZzFngAutoLaserKey, 0, 512, 1,
			nullptr, 0, "toggle cl_zz_fng_auto_laser 0 1", &CConfig::m_ClZzFngAutoLaserMod},
		{ERowType::KEYBIND, "Auto unfreeze", &CConfig::m_ClZzAutoUnfreezeKey, 0, 512, 1,
			nullptr, 0, "toggle cl_zz_auto_unfreeze 0 1", &CConfig::m_ClZzAutoUnfreezeMod},
		{ERowType::KEYBIND, "Avoid", &CConfig::m_ClZzAvoidToggleKey, 0, 512, 1,
			nullptr, 0, "toggle cl_zz_avoid_enabled 0 1", &CConfig::m_ClZzAvoidToggleMod},
		{ERowType::KEYBIND, "Hook drive", &CConfig::m_ClZzFlyRideKey, 0, 512, 1,
			nullptr, 0, "toggle cl_zz_fly_ride 0 1", &CConfig::m_ClZzFlyRideMod},
		{ERowType::KEYBIND, "Grenade save", &CConfig::m_ClKnGrenadeSaveKey, 0, 512, 1,
			nullptr, 0, "toggle cl_kn_10 0 1", &CConfig::m_ClKnGrenadeSaveMod},
		{ERowType::KEYBIND, "Observers", &CConfig::m_ClKnWatcherListKey, 0, 512, 1,
			nullptr, 0, "toggle cl_kn_04 0 1", &CConfig::m_ClKnWatcherListMod},
		{ERowType::KEYBIND, "Weapon prediction", &CConfig::m_ClKnWeaponPredictionKey, 0, 512, 1,
			nullptr, 0, "toggle cl_kn_02 0 1", &CConfig::m_ClKnWeaponPredictionMod},
	};

	struct SPanel
	{
		const char *m_pTitle;
		const char *m_pIcon;
		SRow *m_pRows;
		int m_NumRows;
	};

	static SPanel s_aPanels[] = {
		{"Combat", FontIcon::MAGNIFYING_GLASS, s_aAimRows, (int)std::size(s_aAimRows)},
		{"FNG Laser", FontIcon::STAR, s_aFngRows, (int)std::size(s_aFngRows)},
		{"Movement", FontIcon::ARROWS_LEFT_RIGHT, s_aMovementRows, (int)std::size(s_aMovementRows)},
		{"Visuals", FontIcon::LAYER_GROUP, s_aUtilityRows, (int)std::size(s_aUtilityRows)},
		{"Other", FontIcon::USER, s_aKernelNetRows, (int)std::size(s_aKernelNetRows)},
		{"Binds", FontIcon::KEY, s_aBindRows, (int)std::size(s_aBindRows)},
	};

	float PanelBodyHeight(int Panel)
	{
		float Height = 0.0f;
		for(int i = 0; i < s_aPanels[Panel].m_NumRows; ++i)
			Height += RowHeight(s_aPanels[Panel].m_pRows[i].m_Type) + 3.0f;
		return Height + 8.0f;
	}

	float SmoothFactor(float DeltaTime, float Response)
	{
		return 1.0f - std::exp(-std::clamp(DeltaTime, 0.0f, 0.05f) * Response);
	}

	float SmoothValue(float Current, float Target, float DeltaTime,
		float Response)
	{
		return Current + (Target - Current) * SmoothFactor(DeltaTime, Response);
	}

	vec2 SmoothPosition(vec2 Current, vec2 Target, float DeltaTime,
		float Response)
	{
		return Current + (Target - Current) * SmoothFactor(DeltaTime, Response);
	}

	float Hash01(uint32_t Value)
	{
		Value ^= Value >> 16;
		Value *= 0x7feb352dU;
		Value ^= Value >> 15;
		Value *= 0x846ca68bU;
		Value ^= Value >> 16;
		return (Value & 0x00ffffffU) / 16777215.0f;
	}
} // namespace

void CZZClickGui::OnReset()
{
	m_Anim = 0.0f;
	m_OpenElapsed = 0.0f;
	m_WasOpen = false;
	m_OpenedOverMenu = false;
	m_PositionsInitialized = false;
	m_DraggingPanel = false;
	m_DragPanel = -1;
	m_DragSliderPanel = -1;
	m_DragSliderRow = -1;
	m_WaitingForBind = false;
	m_pWaitingBind = nullptr;
	m_ThemePulse = 0.0f;
	m_UiScale = 1.0f;
	m_LastScreenWidth = 0;
	m_LastScreenHeight = 0;
	m_LastTheme = g_Config.m_ClZzTheme;
	m_RenderPalette = GetZZThemePalette(
		g_Config.m_ClZzTheme,
		color_cast<ColorRGBA>(ColorHSLA(g_Config.m_UiColor, true)));
	for(int i = 0; i < NUM_PANELS; ++i)
	{
		m_aCollapsed[i] = false;
		m_aZOrder[i] = i;
		m_aPanelVisualPos[i] = vec2(0.0f, 0.0f);
		m_aPanelVisualAlpha[i] = 0.0f;
		m_aPanelScroll[i] = 0.0f;
		m_aPanelBodyViewportHeight[i] = 0.0f;
		m_aCollapseAnim[i] = 1.0f;
		m_aPanelHoverAnim[i] = 0.0f;
		for(int Row = 0; Row < MAX_PANEL_ROWS; ++Row)
		{
			m_aaRowHoverAnim[i][Row] = 0.0f;
			m_aaToggleAnim[i][Row] =
				Row < s_aPanels[i].m_NumRows &&
						s_aPanels[i].m_pRows[Row].m_Type == ERowType::TOGGLE ?
					(float)(g_Config.*(s_aPanels[i].m_pRows[Row].m_pValue) != 0) :
					0.0f;
			if(Row < s_aPanels[i].m_NumRows &&
				s_aPanels[i].m_pRows[Row].m_Type == ERowType::SLIDER)
			{
				const SRow &Slider = s_aPanels[i].m_pRows[Row];
				m_aaSliderAnim[i][Row] =
					Slider.m_Max == Slider.m_Min ? 0.0f : (float)((g_Config.*(Slider.m_pValue)) - Slider.m_Min) / (float)(Slider.m_Max - Slider.m_Min);
			}
			else
				m_aaSliderAnim[i][Row] = 0.0f;
		}
	}
}

void CZZClickGui::InitializePositions(float ScreenW, float ScreenH)
{
	const float Gap = 16.0f;
	const bool MultipleRows =
		ScreenW < PANEL_WIDTH * NUM_PANELS + Gap * (NUM_PANELS + 1);
	const int Columns = MultipleRows ?
		(ScreenW < PANEL_WIDTH * 3.0f ? 2 : 3) :
		NUM_PANELS;
	const int Rows = (NUM_PANELS + Columns - 1) / Columns;
	const float TotalWidth = Columns * PANEL_WIDTH + (Columns - 1) * Gap;
	const float StartX = std::max(14.0f, (ScreenW - TotalWidth) * 0.5f);
	const float TopMargin = 16.0f;
	const float BottomMargin = 14.0f;
	const float RowGap = 12.0f;
	const float RowSlotHeight = std::max(HEADER_HEIGHT,
		(ScreenH - TopMargin - BottomMargin - (Rows - 1) * RowGap) / Rows);
	const float BodyViewportHeight = std::max(0.0f, RowSlotHeight - HEADER_HEIGHT);
	for(int i = 0; i < NUM_PANELS; ++i)
	{
		const int Column = i % Columns;
		const int Row = i / Columns;
		m_aPanelPos[i] = vec2(StartX + Column * (PANEL_WIDTH + Gap),
			TopMargin + Row * (RowSlotHeight + RowGap));
		m_aPanelRenderPos[i] = m_aPanelPos[i];
		m_aPanelVisualPos[i] = m_aPanelPos[i];
		m_aPanelVisualAlpha[i] = 0.0f;
		m_aPanelBodyViewportHeight[i] = BodyViewportHeight;
		m_aPanelScroll[i] = 0.0f;
	}
	m_PositionsInitialized = true;
}

bool CZZClickGui::PointInRect(vec2 Point, float X, float Y, float W,
	float H) const
{
	return Point.x >= X && Point.x <= X + W && Point.y >= Y && Point.y <= Y + H;
}

void CZZClickGui::BringToFront(int Panel)
{
	const int OldOrder = m_aZOrder[Panel];
	for(int i = 0; i < NUM_PANELS; ++i)
	{
		if(m_aZOrder[i] > OldOrder)
			--m_aZOrder[i];
	}
	m_aZOrder[Panel] = NUM_PANELS - 1;
}

float CZZClickGui::VisiblePanelBodyHeight(int Panel, vec2 Pos) const
{
	const float LogicalScreenH =
		Graphics()->ScreenHeight() / std::max(m_UiScale, 0.001f);
	const float SpaceBelow = std::max(0.0f,
		LogicalScreenH - Pos.y - HEADER_HEIGHT - 12.0f);
	return std::min({PanelBodyHeight(Panel), m_aPanelBodyViewportHeight[Panel],
		SpaceBelow});
}

void CZZClickGui::UpdateMousePosition()
{
	if(!m_OpenedOverMenu)
		m_MousePos = Input()->NativeMousePos() / std::max(m_UiScale, 0.001f);
}

bool CZZClickGui::ScrollHoveredPanel(float Amount)
{
	int Order[NUM_PANELS];
	for(int Panel = 0; Panel < NUM_PANELS; ++Panel)
		Order[Panel] = Panel;
	std::sort(std::begin(Order), std::end(Order),
		[this](int A, int B) { return m_aZOrder[A] > m_aZOrder[B]; });

	for(int Index = 0; Index < NUM_PANELS; ++Index)
	{
		const int Panel = Order[Index];
		if(m_aPanelVisualAlpha[Panel] <= 0.01f)
			continue;
		const vec2 Pos = m_aPanelVisualPos[Panel];
		const float BodyHeight = VisiblePanelBodyHeight(Panel, Pos);
		if(!PointInRect(m_MousePos, Pos.x, Pos.y + HEADER_HEIGHT, PANEL_WIDTH,
			   BodyHeight))
			continue;

		const float MaxScroll =
			std::max(0.0f, PanelBodyHeight(Panel) - BodyHeight);
		if(MaxScroll <= 0.0f)
			return false;

		m_aPanelScroll[Panel] =
			std::clamp(m_aPanelScroll[Panel] + Amount, 0.0f, MaxScroll);
		BringToFront(Panel);
		return true;
	}
	return false;
}

void CZZClickGui::CloseClickGui()
{
	g_Config.m_ClZzClickGui = 0;
	m_Anim = 0.0f;
	m_OpenElapsed = 0.0f;
	m_WaitingForBind = false;
	m_pWaitingBind = nullptr;
	HandleMouseUp();
	if(GameClient()->m_Menus.IsActive())
		Input()->MouseModeRelative();
	else if(!GameClient()->m_GameConsole.IsActive() && !GameClient()->m_Chat.IsActive())
		Input()->MouseModeRelative();
	m_OpenedOverMenu = false;
}

void CZZClickGui::DrawText(float X, float Y, float Size, const char *pText,
	const ColorRGBA &Color, bool RightAligned)
{
	if(RightAligned)
		X -= TextRender()->TextWidth(Size, pText);
	CTextCursor Cursor;
	Cursor.SetPosition(vec2(X, Y));
	Cursor.m_FontSize = Size;
	TextRender()->TextColor(Color);
	TextRender()->TextEx(&Cursor, pText, -1);
}

void CZZClickGui::RenderPanel(int Panel, vec2 Pos, float Alpha)
{
	const SPanel &PanelData = s_aPanels[Panel];
	const SZZThemePalette &Palette = m_RenderPalette;
	const bool Animate = g_Config.m_ClZzClickGuiAnimations != 0;
	const float DeltaTime = std::clamp(Client()->RenderFrameTime(), 0.0f, 0.05f);
	const float Response = 13.0f * (g_Config.m_ClZzAnimationSpeed / 100.0f);
	// One exponential per panel instead of one per animated row/property.
	const float AnimationBlend =
		Animate ? SmoothFactor(DeltaTime, Response) : 1.0f;
	auto Approach = [AnimationBlend](float Current, float Target) {
		return Current + (Target - Current) * AnimationBlend;
	};
	// Fixed panels cannot be collapsed. This prevents the unusable header-only
	// state and keeps all feature groups visible every time the GUI opens.
	m_aCollapsed[Panel] = false;
	m_aCollapseAnim[Panel] = Approach(m_aCollapseAnim[Panel], 1.0f);
	const float FullBodyHeight = PanelBodyHeight(Panel);
	const float VisibleFullBodyHeight = VisiblePanelBodyHeight(Panel, Pos);
	const float MaxScroll =
		std::max(0.0f, FullBodyHeight - VisibleFullBodyHeight);
	m_aPanelScroll[Panel] =
		std::clamp(m_aPanelScroll[Panel], 0.0f, MaxScroll);
	const float BodyHeight = VisibleFullBodyHeight * m_aCollapseAnim[Panel];
	const bool PanelHovered = PointInRect(m_MousePos, Pos.x, Pos.y, PANEL_WIDTH,
		HEADER_HEIGHT + BodyHeight);
	m_aPanelHoverAnim[Panel] =
		Approach(m_aPanelHoverAnim[Panel], PanelHovered ? 1.0f : 0.0f);
	const float HoverGlow = m_aPanelHoverAnim[Panel];
	// The theme colors the glass itself, not only labels and controls. A neutral
	// silver component keeps the iOS-like material readable on every map.
	const ColorRGBA GlassBase = ColorRGBA(0.29f, 0.31f, 0.35f, 1.0f);
	const ColorRGBA ThemedGlass = ZZThemeLerp(Palette.m_Panel, Palette.m_Accent, 0.14f);
	const ColorRGBA GlassTint = ZZThemeLerp(GlassBase, ThemedGlass, 0.58f);
	const ColorRGBA Border = ZZThemeLerp(
		ColorRGBA(0.96f, 0.98f, 1.0f, 1.0f), Palette.m_Accent, 0.62f)
					 .WithAlpha((0.31f + HoverGlow * 0.15f) * Alpha);
	const ColorRGBA Background =
		GlassTint.WithAlpha((0.58f + HoverGlow * 0.055f) * Alpha);
	auto DrawCenteredIcon = [&](float X, float Y, float W, float H, float Size,
					const char *pIcon, const ColorRGBA &Color) {
		TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
		const STextBoundingBox Bounds = TextRender()->TextBoundingBox(Size, pIcon);
		TextRender()->TextColor(Color);
		TextRender()->Text(X + (W - Bounds.m_W) * 0.5f, Y + (H - Bounds.m_H) * 0.5f,
			Size, pIcon);
		TextRender()->TextColor(TextRender()->DefaultTextColor());
		TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
	};

	const float PanelHeight = HEADER_HEIGHT + BodyHeight;
	// Low-alpha concentric shells form a symmetric neon edge, without the old
	// offset black shadow. The glow follows the selected theme.
	Graphics()->DrawRect(Pos.x - 5.0f, Pos.y - 5.0f, PANEL_WIDTH + 10.0f,
		PanelHeight + 10.0f,
		Palette.m_Accent.WithAlpha((0.018f + HoverGlow * 0.018f) * Alpha),
		IGraphics::CORNER_ALL, 27.0f);
	Graphics()->DrawRect(Pos.x - 3.0f, Pos.y - 3.0f, PANEL_WIDTH + 6.0f,
		PanelHeight + 6.0f,
		Palette.m_AccentAlt.WithAlpha((0.026f + HoverGlow * 0.024f) * Alpha),
		IGraphics::CORNER_ALL, 25.0f);
	Graphics()->DrawRect(Pos.x - 1.0f, Pos.y - 1.0f, PANEL_WIDTH + 2.0f,
		PanelHeight + 2.0f, Border, IGraphics::CORNER_ALL, 23.0f);
	Graphics()->DrawRect(Pos.x, Pos.y, PANEL_WIDTH, PanelHeight,
		Background, IGraphics::CORNER_ALL, 22.0f);
	// A translucent inner membrane keeps the map visible through the glass.
	Graphics()->DrawRect(Pos.x + 1.0f, Pos.y + 1.0f, PANEL_WIDTH - 2.0f,
		PanelHeight - 2.0f,
		ZZThemeLerp(ColorRGBA(0.96f, 0.98f, 1.0f, 1.0f), Palette.m_AccentAlt, 0.22f)
			.WithAlpha(0.045f * Alpha),
		IGraphics::CORNER_ALL, 21.0f);

	// Soft rounded refraction pools replace flat gradients and remain inside the
	// curved surface even on renderers without a rounded stencil clip.
	Graphics()->DrawRect(Pos.x + 6.0f, Pos.y + 4.0f, PANEL_WIDTH * 0.53f,
		52.0f,
		ZZThemeLerp(ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), Palette.m_AccentAlt, 0.12f)
			.WithAlpha(0.075f * Alpha),
		IGraphics::CORNER_ALL, 20.0f);
	Graphics()->DrawRect(Pos.x + PANEL_WIDTH * 0.70f, Pos.y + 5.0f,
		PANEL_WIDTH * 0.24f, PanelHeight - 10.0f,
		Palette.m_AccentAlt.WithAlpha((0.025f + HoverGlow * 0.018f) * Alpha),
		IGraphics::CORNER_ALL, 20.0f);
	const float AccentPoolH = std::clamp(PanelHeight * 0.26f, 18.0f, 96.0f);
	Graphics()->DrawRect(Pos.x + PANEL_WIDTH * 0.55f,
		Pos.y + PanelHeight - AccentPoolH - 7.0f, PANEL_WIDTH * 0.39f,
		AccentPoolH, Palette.m_Accent.WithAlpha(0.050f * Alpha),
		IGraphics::CORNER_ALL, minimum(34.0f, AccentPoolH * 0.5f));

	// The title belongs to the same glass sheet as the body, like the reference.
	Graphics()->DrawRect(Pos.x + 7.0f, Pos.y + HEADER_HEIGHT - 1.0f,
		PANEL_WIDTH - 14.0f, 1.0f,
		ColorRGBA(1.0f, 1.0f, 1.0f, 0.10f * Alpha),
		IGraphics::CORNER_ALL, 0.5f);
	const float IconX = Pos.x + 8.0f;
	const float IconY = Pos.y + 7.0f;
	Graphics()->DrawRect(IconX, IconY, 18.0f, 18.0f,
		ColorRGBA(1.0f, 1.0f, 1.0f, (0.075f + HoverGlow * 0.030f) * Alpha),
		IGraphics::CORNER_ALL, 9.0f);
	DrawCenteredIcon(IconX, IconY, 18.0f, 18.0f, 9.0f, PanelData.m_pIcon,
		Palette.m_Text.WithAlpha((0.82f + HoverGlow * 0.18f) * Alpha));
	DrawText(Pos.x + 31.0f + HoverGlow, Pos.y + 8.0f, 13.5f,
		PanelData.m_pTitle, Palette.m_Text.WithAlpha(Alpha));

	if(BodyHeight <= 0.01f)
		return;

	// Clip coordinates are framebuffer pixels whereas the ClickGUI uses its
	// scaled design canvas, hence the explicit conversion by m_UiScale.
	Graphics()->ClipEnable((int)std::lround(Pos.x * m_UiScale),
		(int)std::lround((Pos.y + HEADER_HEIGHT) * m_UiScale),
		(int)std::lround(PANEL_WIDTH * m_UiScale),
		(int)std::ceil((BodyHeight + 1.0f) * m_UiScale));
	float Y = Pos.y + HEADER_HEIGHT + BODY_PADDING - m_aPanelScroll[Panel];
	for(int RowIndex = 0; RowIndex < PanelData.m_NumRows; ++RowIndex)
	{
		const SRow &Row = PanelData.m_pRows[RowIndex];
		const float Height = RowHeight(Row.m_Type);
		const bool Hovered = PointInRect(m_MousePos, Pos.x + BODY_PADDING, Y,
			PANEL_WIDTH - BODY_PADDING * 2.0f, Height);
		m_aaRowHoverAnim[Panel][RowIndex] =
			Approach(m_aaRowHoverAnim[Panel][RowIndex], Hovered ? 1.0f : 0.0f);
		const float Hover = m_aaRowHoverAnim[Panel][RowIndex];
		const float Stagger =
			Animate ? std::clamp(m_aCollapseAnim[Panel] * 1.3f - RowIndex * 0.018f,
					  0.0f, 1.0f) :
				  m_aCollapseAnim[Panel];
		const float RowAlpha = Alpha * Stagger;
		const float DrawY = Y + (1.0f - Stagger) * -7.0f;
		if(Hover > 0.001f)
		{
			const float RowX = Pos.x + BODY_PADDING;
			const float RowW = PANEL_WIDTH - BODY_PADDING * 2.0f;
			Graphics()->DrawRect(
				RowX, DrawY, RowW, Height,
				Palette.m_PanelHover.WithAlpha(0.25f * Hover * RowAlpha),
				IGraphics::CORNER_ALL, 8.0f);
			Graphics()->DrawRect(RowX + 1.0f, DrawY + 1.0f, RowW - 2.0f,
				Height - 2.0f,
				ColorRGBA(1.0f, 1.0f, 1.0f, 0.030f * Hover * RowAlpha),
				IGraphics::CORNER_ALL, 7.0f);
		}
		if(Row.m_Type == ERowType::ACTION)
		{
			const float ButtonX = Pos.x + BODY_PADDING + 2.0f;
			const float ButtonW = PANEL_WIDTH - BODY_PADDING * 2.0f - 4.0f;
			Graphics()->DrawRect(ButtonX, DrawY + 2.0f, ButtonW, Height - 4.0f,
				Palette.m_Accent.WithAlpha((0.22f + Hover * 0.20f) * RowAlpha),
				IGraphics::CORNER_ALL, (Height - 4.0f) * 0.5f);
			Graphics()->DrawRect(ButtonX + 1.0f, DrawY + 3.0f, ButtonW - 2.0f,
				Height - 6.0f,
				ColorRGBA(1.0f, 1.0f, 1.0f, (0.040f + Hover * 0.035f) * RowAlpha),
				IGraphics::CORNER_ALL, (Height - 6.0f) * 0.5f);
			const float TextW = TextRender()->TextWidth(13.0f, Row.m_pName);
			DrawText(Pos.x + (PANEL_WIDTH - TextW) * 0.5f, DrawY + 5.0f, 13.0f,
				Row.m_pName, Palette.m_Text.WithAlpha(RowAlpha));
			Y += Height + ROW_GAP;
			continue;
		}

		int &Value = g_Config.*(Row.m_pValue);

		if(Row.m_Type == ERowType::TOGGLE)
		{
			const float SwitchW = 28.0f;
			const float SwitchH = 16.0f;
			const float SwitchX = Pos.x + PANEL_WIDTH - BODY_PADDING - SwitchW - 4.0f;
			const float SwitchY = DrawY + 5.0f;
			m_aaToggleAnim[Panel][RowIndex] =
				Approach(m_aaToggleAnim[Panel][RowIndex], Value ? 1.0f : 0.0f);
			const float Toggle = m_aaToggleAnim[Panel][RowIndex];
			const ColorRGBA SwitchColor = ZZThemeLerp(
				ZZThemeLerp(GlassBase, Palette.m_Panel, 0.55f), Palette.m_Accent, Toggle);
			Graphics()->DrawRect(SwitchX, SwitchY, SwitchW, SwitchH,
				ColorRGBA(1.0f, 1.0f, 1.0f, 0.12f * RowAlpha),
				IGraphics::CORNER_ALL, SwitchH * 0.5f);
			Graphics()->DrawRect(SwitchX + 1.0f, SwitchY + 1.0f,
				SwitchW - 2.0f, SwitchH - 2.0f,
				SwitchColor.WithAlpha((0.25f + Toggle * 0.57f) * RowAlpha),
				IGraphics::CORNER_ALL, (SwitchH - 2.0f) * 0.5f);
			const float KnobX = SwitchX + 2.0f + Toggle * (SwitchW - 15.0f);
			Graphics()->DrawRect(KnobX, SwitchY + 2.0f, 12.0f, 12.0f,
				ColorRGBA(0.97f, 0.98f, 1.0f, RowAlpha),
				IGraphics::CORNER_ALL, 6.0f);
			Graphics()->DrawRect(KnobX + 2.0f, SwitchY + 3.0f, 8.0f, 1.5f,
				ColorRGBA(1.0f, 1.0f, 1.0f, 0.42f * RowAlpha),
				IGraphics::CORNER_ALL, 1.0f);
			DrawText(Pos.x + BODY_PADDING + 5.0f + Hover, DrawY + 5.0f, 14.0f,
				Row.m_pName, Palette.m_Text.WithAlpha(RowAlpha));
		}
		else if(Row.m_Type == ERowType::MODE)
		{
			DrawText(Pos.x + BODY_PADDING + 5.0f + Hover * 2.0f, DrawY + 5.0f, 14.0f,
				Row.m_pName, Palette.m_Text.WithAlpha(RowAlpha));
			const int Option = std::clamp(Value, 0, Row.m_NumOptions - 1);
			const float ValueW = TextRender()->TextWidth(12.5f, Row.m_ppOptions[Option]) + 12.0f;
			const float ValueX = Pos.x + PANEL_WIDTH - BODY_PADDING - ValueW - 3.0f;
			Graphics()->DrawRect(ValueX, DrawY + 4.0f, ValueW, 19.0f,
				ColorRGBA(1.0f, 1.0f, 1.0f, 0.085f * RowAlpha),
				IGraphics::CORNER_ALL, 9.5f);
			Graphics()->DrawRect(ValueX + 1.0f, DrawY + 5.0f, ValueW - 2.0f, 17.0f,
				Palette.m_Accent.WithAlpha(0.10f * RowAlpha),
				IGraphics::CORNER_ALL, 8.5f);
			DrawText(Pos.x + PANEL_WIDTH - BODY_PADDING - 7.0f, DrawY + 5.0f, 13.0f,
				Row.m_ppOptions[Option], Palette.m_AccentAlt.WithAlpha(RowAlpha),
				true);
		}
		else if(Row.m_Type == ERowType::KEYBIND)
		{
			DrawText(Pos.x + BODY_PADDING + 5.0f + Hover * 2.0f, DrawY + 5.0f, 14.0f,
				Row.m_pName, Palette.m_Text.WithAlpha(RowAlpha));
			const char *pKeyName = m_WaitingForBind &&
							       m_pWaitingBind == Row.m_pValue ?
						       "Press a key..." :
						       Input()->KeyName(Value);
			const float KeyW = TextRender()->TextWidth(12.5f, pKeyName) + 12.0f;
			const float KeyX = Pos.x + PANEL_WIDTH - BODY_PADDING - KeyW - 3.0f;
			Graphics()->DrawRect(KeyX, DrawY + 4.0f, KeyW, 19.0f,
				ColorRGBA(1.0f, 1.0f, 1.0f, 0.085f * RowAlpha),
				IGraphics::CORNER_ALL, 9.5f);
			Graphics()->DrawRect(KeyX + 1.0f, DrawY + 5.0f, KeyW - 2.0f, 17.0f,
				Palette.m_Panel.WithAlpha(0.34f * RowAlpha),
				IGraphics::CORNER_ALL, 8.5f);
			DrawText(Pos.x + PANEL_WIDTH - BODY_PADDING - 7.0f, DrawY + 5.0f, 13.0f,
				pKeyName, Palette.m_AccentAlt.WithAlpha(RowAlpha), true);
		}
		else
		{
			char aValue[32];
			str_format(aValue, sizeof(aValue), "%d", Value);
			DrawText(Pos.x + BODY_PADDING + 5.0f + Hover * 2.0f, DrawY + 2.0f, 13.5f,
				Row.m_pName, Palette.m_Text.WithAlpha(RowAlpha));
			DrawText(Pos.x + PANEL_WIDTH - BODY_PADDING - 6.0f, DrawY + 2.0f, 13.5f,
				aValue, Palette.m_AccentAlt.WithAlpha(RowAlpha), true);
			const float TrackX = Pos.x + BODY_PADDING + 5.0f;
			const float TrackY = DrawY + 26.0f;
			const float TrackW = PANEL_WIDTH - BODY_PADDING * 2.0f - 10.0f;
			const float TargetPercent =
				Row.m_Max == Row.m_Min ? 0.0f : (float)(Value - Row.m_Min) / (float)(Row.m_Max - Row.m_Min);
			m_aaSliderAnim[Panel][RowIndex] =
				Approach(m_aaSliderAnim[Panel][RowIndex], TargetPercent);
			const float Percent =
				std::clamp(m_aaSliderAnim[Panel][RowIndex], 0.0f, 1.0f);
			Graphics()->DrawRect(TrackX, TrackY - 1.0f, TrackW, 5.0f,
				ColorRGBA(1.0f, 1.0f, 1.0f, 0.10f * RowAlpha),
				IGraphics::CORNER_ALL, 2.5f);
			Graphics()->DrawRect(TrackX + 1.0f, TrackY, TrackW - 2.0f, 3.0f,
				Palette.m_Panel.WithAlpha(0.62f * RowAlpha),
				IGraphics::CORNER_ALL, 1.5f);
			Graphics()->DrawRect(TrackX + 1.0f, TrackY,
				maximum(0.0f, (TrackW - 2.0f) * Percent), 3.0f,
				Palette.m_Accent.WithAlpha(0.78f * RowAlpha),
				IGraphics::CORNER_ALL, 1.5f);
			const float KnobPulse =
				Animate ? Hover * (1.0f + 0.5f * sinf(LocalTime() * 8.0f + RowIndex)) : 0.0f;
			Graphics()->DrawRect(TrackX + TrackW * Percent - 5.0f - KnobPulse,
				TrackY - 4.0f - KnobPulse, 10.0f + KnobPulse * 2.0f,
				11.0f + KnobPulse * 2.0f,
				ColorRGBA(0.97f, 0.98f, 1.0f, RowAlpha),
				IGraphics::CORNER_ALL, 5.5f + KnobPulse);
		}
		Y += Height + ROW_GAP;
	}
	Graphics()->ClipDisable();
}

void CZZClickGui::UpdateSlider(int Panel, int RowIndex)
{
	SRow &Row = s_aPanels[Panel].m_pRows[RowIndex];
	if(Row.m_Type != ERowType::SLIDER)
		return;
	const float TrackX = m_aPanelVisualPos[Panel].x + BODY_PADDING + 5.0f;
	const float TrackW = PANEL_WIDTH - BODY_PADDING * 2.0f - 10.0f;
	const float Percent =
		std::clamp((m_MousePos.x - TrackX) / TrackW, 0.0f, 1.0f);
	const float RawValue = Row.m_Min + Percent * (Row.m_Max - Row.m_Min);
	int Value = Row.m_Min +
		    (int)std::round((RawValue - Row.m_Min) / Row.m_Step) * Row.m_Step;
	Value = std::clamp(Value, Row.m_Min, Row.m_Max);
	g_Config.*(Row.m_pValue) = Value;
	m_aaSliderAnim[Panel][RowIndex] =
		Row.m_Max == Row.m_Min ? 0.0f : (float)(Value - Row.m_Min) / (float)(Row.m_Max - Row.m_Min);
}

void CZZClickGui::HandleMouseDown()
{
	UpdateMousePosition();
	int Order[NUM_PANELS];
	for(int Panel = 0; Panel < NUM_PANELS; ++Panel)
		Order[Panel] = Panel;
	std::sort(std::begin(Order), std::end(Order),
		[this](int A, int B) { return m_aZOrder[A] > m_aZOrder[B]; });
	for(int Index = 0; Index < NUM_PANELS; ++Index)
	{
		const int Panel = Order[Index];
		if(m_aPanelVisualAlpha[Panel] <= 0.01f)
			continue;
		const vec2 Pos = m_aPanelVisualPos[Panel];
		if(PointInRect(m_MousePos, Pos.x, Pos.y, PANEL_WIDTH, HEADER_HEIGHT))
		{
			BringToFront(Panel);
			return;
		}

		const float BodyHeight = VisiblePanelBodyHeight(Panel, Pos);
		if(!PointInRect(m_MousePos, Pos.x, Pos.y + HEADER_HEIGHT, PANEL_WIDTH,
			   BodyHeight))
			continue;

		float Y = Pos.y + HEADER_HEIGHT + BODY_PADDING - m_aPanelScroll[Panel];
		for(int RowIndex = 0; RowIndex < s_aPanels[Panel].m_NumRows; ++RowIndex)
		{
			SRow &Row = s_aPanels[Panel].m_pRows[RowIndex];
			const float Height = RowHeight(Row.m_Type);
			if(PointInRect(m_MousePos, Pos.x + BODY_PADDING, Y,
				   PANEL_WIDTH - BODY_PADDING * 2.0f, Height))
			{
				BringToFront(Panel);
				if(Row.m_Type == ERowType::ACTION)
				{
					ApplyZZVanillaVisuals();
					return;
				}
				int &Value = g_Config.*(Row.m_pValue);
				if(Row.m_Type == ERowType::TOGGLE)
				{
					Value = Value ? 0 : 1;
					if(Row.m_pValue == &CConfig::m_ClZzAvoidEnabled && Value)
						g_Config.m_ClZzAvoidFreeze = 1;
				}
				else if(Row.m_Type == ERowType::MODE)
				{
					Value = (Value + 1) % Row.m_NumOptions;
					if(Row.m_pValue == &CConfig::m_ClZzTheme)
						GameClient()->ApplyZZTheme();
				}
				else if(Row.m_Type == ERowType::KEYBIND)
				{
					m_WaitingForBind = true;
					m_pWaitingBind = Row.m_pValue;
				}
				else
				{
					m_DragSliderPanel = Panel;
					m_DragSliderRow = RowIndex;
					UpdateSlider(Panel, RowIndex);
				}
				return;
			}
			Y += Height + ROW_GAP;
		}
	}
}

void CZZClickGui::HandleMouseUp()
{
	m_DraggingPanel = false;
	m_DragPanel = -1;
	m_DragSliderPanel = -1;
	m_DragSliderRow = -1;
}

bool CZZClickGui::OnInput(const IInput::CEvent &Event)
{
	const bool Open = g_Config.m_ClZzClickGui != 0;
	const bool Pressed = (Event.m_Flags & IInput::FLAG_PRESS) != 0;
	const bool FreshPress =
		Pressed && (Event.m_Flags & IInput::FLAG_REPEAT) == 0;

	// Binding capture deliberately has priority over the ClickGUI toggle key:
	// this permits assigning the currently configured opening key to another
	// action. Escape only cancels capture and must not leak into the pause menu.
	if(Open && m_WaitingForBind && FreshPress && Event.m_Key != KEY_MOUSE_1)
	{
		m_WaitingForBind = false;
		int CConfig::*pBind = m_pWaitingBind;
		m_pWaitingBind = nullptr;
		if(Event.m_Key == KEY_ESCAPE)
			return true;
		if(Event.m_Key >= KEY_FIRST && Event.m_Key < KEY_LAST)
		{
			const SRow *pBindRow = nullptr;
			for(const SPanel &Panel : s_aPanels)
			{
				for(int RowIndex = 0; RowIndex < Panel.m_NumRows; ++RowIndex)
				{
					const SRow &Row = Panel.m_pRows[RowIndex];
					if(Row.m_Type == ERowType::KEYBIND && Row.m_pValue == pBind)
					{
						pBindRow = &Row;
						break;
					}
				}
				if(pBindRow)
					break;
			}
			const int OldKey = pBind ? g_Config.*pBind : KEY_UNKNOWN;
			const int NewModifier = CBinds::GetModifierMask(Input()) &
						~CBinds::GetModifierMaskOfKey(Event.m_Key);
			if(pBind && pBindRow)
			{
				const int OldModifier = pBindRow->m_pModifier ?
					g_Config.*(pBindRow->m_pModifier) : 0;
				if(pBindRow->m_pBindCommand && OldKey >= KEY_FIRST &&
					OldKey < KEY_LAST)
					GameClient()->m_Binds.Bind(OldKey, "", false,
						OldModifier);
				g_Config.*pBind = Event.m_Key;
				if(pBindRow->m_pModifier)
					g_Config.*(pBindRow->m_pModifier) = NewModifier;
				if(pBindRow->m_pBindCommand)
					GameClient()->m_Binds.Bind(Event.m_Key,
						pBindRow->m_pBindCommand, false, NewModifier);
			}
		}
		return true;
	}

	const int EventModifierMask = CBinds::GetModifierMask(Input()) &
				      ~CBinds::GetModifierMaskOfKey(Event.m_Key);
	const bool ConfiguredToggleKey =
		g_Config.m_ClZzClickGuiKey != KEY_UNKNOWN &&
		Event.m_Key == g_Config.m_ClZzClickGuiKey &&
		EventModifierMask == g_Config.m_ClZzClickGuiMod;
	const bool OptionalInsertFallback = !g_Config.m_ClZzClickGuiKeyOnly &&
					    Event.m_Key == KEY_INSERT && EventModifierMask == 0;
	// Always honor the ClickGUI's own toggle before passing an event to chat or
	// the console. Otherwise a GUI opened from settings while one of them is
	// active cannot be closed again from its configured key.
	if(FreshPress &&
		(ConfiguredToggleKey || OptionalInsertFallback))
	{
		if(Open)
			CloseClickGui();
		else
		{
			g_Config.m_ClZzClickGui = 1;
			m_WaitingForBind = false;
			m_pWaitingBind = nullptr;
		}
		return true;
	}

	// An open ClickGUI owns Escape. Consuming it avoids opening the pause menu
	// behind the GUI merely because the user wanted to close the overlay.
	if(Open && Pressed && Event.m_Key == KEY_ESCAPE)
	{
		CloseClickGui();
		return true;
	}

	// With the ClickGUI closed, chat and console retain their normal priority.
	// With it open, it deliberately captures input even if either one happened
	// to be opened first, so its own controls remain operable.
	if(!Open && (GameClient()->m_Chat.IsActive() ||
			 GameClient()->m_GameConsole.IsActive()))
		return false;
	if(!Open)
		return false;

	if(FreshPress && (Event.m_Key == KEY_MOUSE_WHEEL_UP ||
				  Event.m_Key == KEY_MOUSE_WHEEL_DOWN))
	{
		UpdateMousePosition();
		const float ScrollAmount = Event.m_Key == KEY_MOUSE_WHEEL_DOWN ?
			42.0f :
			-42.0f;
		ScrollHoveredPanel(ScrollAmount);
		return true;
	}
	if(Pressed && Event.m_Key == KEY_MOUSE_1)
	{
		HandleMouseDown();
		return true;
	}
	if((Event.m_Flags & IInput::FLAG_RELEASE) && Event.m_Key == KEY_MOUSE_1)
	{
		HandleMouseUp();
		return true;
	}
	return g_Config.m_ClZzClickGui != 0;
}

bool CZZClickGui::OnCursorMove(float x, float y,
	IInput::ECursorType CursorType)
{
	if(g_Config.m_ClZzClickGui == 0)
		return false;
	if(m_OpenedOverMenu)
	{
		Ui()->ConvertMouseMove(&x, &y, CursorType);
		Ui()->OnCursorMove(x, y);
		const CUIRect *pUiScreen = Ui()->Screen();
		if(pUiScreen->w > 1.0f && pUiScreen->h > 1.0f)
		{
			const vec2 Normalized = (Ui()->MousePos() - vec2(pUiScreen->x, pUiScreen->y)) /
				vec2(pUiScreen->w, pUiScreen->h);
			m_MousePos = Normalized * vec2(Graphics()->ScreenWidth() / m_UiScale,
				Graphics()->ScreenHeight() / m_UiScale);
		}
	}
	else
		UpdateMousePosition();
	return true;
}

void CZZClickGui::OnRender()
{
	const bool Open = g_Config.m_ClZzClickGui != 0;
	if(!Open)
	{
		// The setting can be changed outside this component (for example from
		// settings or the console), so clear interaction state on the closing
		// transition as well. Do not call the SDL mouse-mode path every frame
		// while closed: it can itself introduce frame-time spikes on some drivers.
		const bool WasOpen = m_WasOpen;
		m_Anim = 0.0f;
		m_OpenElapsed = 0.0f;
		m_WaitingForBind = false;
		m_pWaitingBind = nullptr;
		if(WasOpen)
		{
			HandleMouseUp();
			if(GameClient()->m_Menus.IsActive() ||
				(!GameClient()->m_GameConsole.IsActive() && !GameClient()->m_Chat.IsActive()))
				Input()->MouseModeRelative();
		}
		m_OpenedOverMenu = false;
		m_WasOpen = false;
		return;
	}

	const bool JustOpened = !m_WasOpen;
	if(JustOpened)
		m_OpenElapsed = 0.0f;
	m_WasOpen = true;
	const bool Animate = g_Config.m_ClZzClickGuiAnimations != 0;
	const float DeltaTime = std::clamp(Client()->RenderFrameTime(), 0.0f, 0.05f);
	const float SpeedScale = g_Config.m_ClZzAnimationSpeed / 100.0f;
	m_OpenElapsed += DeltaTime * std::clamp(SpeedScale, 0.25f, 2.0f);
	const float TargetAnim = 1.0f;
	m_Anim = Animate ? SmoothValue(m_Anim, TargetAnim, DeltaTime, 8.5f * SpeedScale) : TargetAnim;
	if(std::abs(m_Anim - TargetAnim) < 0.001f)
		m_Anim = TargetAnim;

	const int PhysicalScreenW = Graphics()->ScreenWidth();
	const int PhysicalScreenH = Graphics()->ScreenHeight();
	// Scale the whole ClickGUI from a 1700x960 design canvas. Using the smaller
	// axis keeps the proportions stable on 16:9, 16:10 and ultrawide screens.
	m_UiScale = std::clamp(
		std::min(PhysicalScreenW / 1700.0f, PhysicalScreenH / 960.0f),
		0.60f, 1.80f);
	const float ScreenW = PhysicalScreenW / m_UiScale;
	const float ScreenH = PhysicalScreenH / m_UiScale;
	if(JustOpened)
	{
		m_OpenedOverMenu = GameClient()->m_Menus.IsActive();
		if(m_OpenedOverMenu)
		{
			const CUIRect *pUiScreen = Ui()->Screen();
			if(pUiScreen->w > 1.0f && pUiScreen->h > 1.0f)
				m_MousePos = (Ui()->MousePos() - vec2(pUiScreen->x, pUiScreen->y)) /
					vec2(pUiScreen->w, pUiScreen->h) * vec2(ScreenW, ScreenH);
		}
		else
			Input()->MouseModeAbsolute();
	}
	UpdateMousePosition();
	if(!m_PositionsInitialized || PhysicalScreenW != m_LastScreenWidth ||
		PhysicalScreenH != m_LastScreenHeight)
	{
		InitializePositions(ScreenW, ScreenH);
		m_LastScreenWidth = PhysicalScreenW;
		m_LastScreenHeight = PhysicalScreenH;
	}
	for(int Panel = 0; Panel < NUM_PANELS; ++Panel)
	{
		m_aPanelRenderPos[Panel] =
			Animate ? SmoothPosition(m_aPanelRenderPos[Panel], m_aPanelPos[Panel],
					  DeltaTime, 18.0f * SpeedScale) :
				  m_aPanelPos[Panel];
	}
	if(m_DragSliderPanel >= 0 && m_DragSliderRow >= 0)
		UpdateSlider(m_DragSliderPanel, m_DragSliderRow);

	if(m_LastTheme != g_Config.m_ClZzTheme)
	{
		m_LastTheme = g_Config.m_ClZzTheme;
		m_ThemePulse = 1.0f;
	}
	m_ThemePulse =
		Animate ? SmoothValue(m_ThemePulse, 0.0f, DeltaTime, 4.5f * SpeedScale) : 0.0f;
	const SZZThemePalette TargetPalette = GetZZThemePalette(
		g_Config.m_ClZzTheme,
		color_cast<ColorRGBA>(ColorHSLA(g_Config.m_UiColor, true)));
	const float ThemeBlend =
		Animate ? SmoothFactor(DeltaTime, 7.0f * SpeedScale) : 1.0f;
	m_RenderPalette.m_pName = TargetPalette.m_pName;
	m_RenderPalette.m_Accent =
		ZZThemeLerp(m_RenderPalette.m_Accent, TargetPalette.m_Accent, ThemeBlend);
	m_RenderPalette.m_AccentAlt = ZZThemeLerp(
		m_RenderPalette.m_AccentAlt, TargetPalette.m_AccentAlt, ThemeBlend);
	m_RenderPalette.m_Background = ZZThemeLerp(
		m_RenderPalette.m_Background, TargetPalette.m_Background, ThemeBlend);
	m_RenderPalette.m_Panel =
		ZZThemeLerp(m_RenderPalette.m_Panel, TargetPalette.m_Panel, ThemeBlend);
	m_RenderPalette.m_PanelHover = ZZThemeLerp(
		m_RenderPalette.m_PanelHover, TargetPalette.m_PanelHover, ThemeBlend);
	m_RenderPalette.m_Text =
		ZZThemeLerp(m_RenderPalette.m_Text, TargetPalette.m_Text, ThemeBlend);
	m_RenderPalette.m_TextMuted = ZZThemeLerp(
		m_RenderPalette.m_TextMuted, TargetPalette.m_TextMuted, ThemeBlend);
	const SZZThemePalette &Palette = m_RenderPalette;
	const float Eased = Animate ?
		m_Anim * m_Anim * m_Anim *
			(m_Anim * (m_Anim * 6.0f - 15.0f) + 10.0f) :
		m_Anim;
	const bool BackdropEffects = g_Config.m_ClZzClickGuiBackdropEffects != 0;
	const float BackdropSoftness = std::clamp(
		g_Config.m_ClZzClickGuiBackdropSoftness / 100.0f, 0.0f, 1.0f);
	if(BackdropEffects && Eased > 0.01f)
	{
		// A negative sharpening value selects the optimized 3x3 backdrop blur in
		// the composite shader. Bloom is zero, so this stays a single post-process
		// pass instead of running the expensive bloom pyramid every frame.
		const float VisualStrength = std::clamp(
			g_Config.m_ClZzVisualEffectsIntensity / 100.0f, 0.0f, 1.0f);
		const float BlurAmount =
			(0.52f + BackdropSoftness * 0.43f) * Eased *
			mix(0.88f, 1.05f, VisualStrength);
		Graphics()->PostProcess(0.0f, 1.0f, 0.35f, 1.0f, 1.0f, 1.0f,
			0.0f, 0.0f, 1.0f, -BlurAmount, 1);
	}
	Graphics()->MapScreen(0.0f, 0.0f, ScreenW, ScreenH);
	Graphics()->TextureClear();
	// Keep the world bright: this is only a faint theme cast over the blur.
	const ColorRGBA BackdropTint = ZZThemeLerp(ColorRGBA(0.08f, 0.08f, 0.09f, 1.0f),
		Palette.m_Background, 0.35f);
	Graphics()->DrawRect(
		0.0f, 0.0f, ScreenW, ScreenH,
		BackdropTint.WithAlpha(BackdropEffects ?
			(0.012f + BackdropSoftness * 0.038f) * Eased : 0.0f),
		IGraphics::CORNER_NONE, 0.0f);
	if(BackdropEffects)
	{
		const float EffectStrength =
			BackdropSoftness * Eased *
			std::clamp(g_Config.m_ClZzVisualEffectsIntensity / 100.0f, 0.0f, 1.0f);
		for(int Pass = 0; Pass < 4; ++Pass)
		{
			const float Inset = Pass * 18.0f;
			const float Edge = 26.0f + Pass * 9.0f;
			const float Alpha = (0.002f + Pass * 0.001f) * EffectStrength;
			Graphics()->DrawRect(Inset, Inset, ScreenW - Inset * 2.0f, Edge,
				ColorRGBA(0.0f, 0.0f, 0.0f, Alpha),
				IGraphics::CORNER_NONE, 0.0f);
			Graphics()->DrawRect(
				Inset, ScreenH - Inset - Edge, ScreenW - Inset * 2.0f, Edge,
				ColorRGBA(0.0f, 0.0f, 0.0f, Alpha), IGraphics::CORNER_NONE, 0.0f);
			Graphics()->DrawRect(
				Inset, Inset + Edge, Edge, ScreenH - (Inset + Edge) * 2.0f,
				ColorRGBA(0.0f, 0.0f, 0.0f, Alpha), IGraphics::CORNER_NONE, 0.0f);
			Graphics()->DrawRect(ScreenW - Inset - Edge, Inset + Edge, Edge,
				ScreenH - (Inset + Edge) * 2.0f,
				ColorRGBA(0.0f, 0.0f, 0.0f, Alpha),
				IGraphics::CORNER_NONE, 0.0f);
		}

		const float BackdropTime = LocalTime() * SpeedScale;
		IGraphics::CFreeformItem aAccentBands[2];
		IGraphics::CFreeformItem aAltBands[2];
		for(int i = 0; i < 2; ++i)
		{
			const float Travel = ScreenW + 520.0f;
			const float X =
				fmodf(BackdropTime * (18.0f + i * 7.0f) + i * ScreenW * 0.57f,
					Travel) -
				260.0f;
			const float Width = 120.0f + i * 52.0f;
			aAccentBands[i] =
				IGraphics::CFreeformItem(X - 90.0f, 0.0f, X + Width, 0.0f,
					X + Width + 90.0f, ScreenH, X, ScreenH);
			const float AltX = ScreenW - X - Width;
			aAltBands[i] = IGraphics::CFreeformItem(AltX, 0.0f, AltX + Width, 0.0f,
				AltX + Width - 90.0f, ScreenH,
				AltX - 90.0f, ScreenH);
		}
		Graphics()->QuadsBegin();
		Graphics()->SetColor(Palette.m_Accent.WithAlpha(0.002f * EffectStrength));
		Graphics()->QuadsDrawFreeform(aAccentBands, std::size(aAccentBands));
		Graphics()->SetColor(
			Palette.m_AccentAlt.WithAlpha(0.0015f * EffectStrength));
		Graphics()->QuadsDrawFreeform(aAltBands, std::size(aAltBands));
		Graphics()->QuadsEnd();

		IGraphics::CLineItem aBackdropStreaks[12];
		for(int i = 0; i < (int)std::size(aBackdropStreaks); ++i)
		{
			const float SeedX = Hash01((uint32_t)i * 53U + 13U);
			const float SeedY = Hash01((uint32_t)i * 97U + 29U);
			const float Travel = ScreenW + 100.0f;
			const float X =
				fmodf(SeedX * Travel + BackdropTime * (12.0f + SeedY * 26.0f),
					Travel) -
				50.0f;
			const float Y = SeedY * ScreenH;
			const float Length = 18.0f + SeedX * 42.0f;
			aBackdropStreaks[i] =
				IGraphics::CLineItem(X, Y, X + Length, Y - 3.0f - SeedY * 5.0f);
		}
		Graphics()->LinesBegin();
		Graphics()->SetColor(
			Palette.m_AccentAlt.WithAlpha(0.010f * EffectStrength));
		Graphics()->LinesDraw(aBackdropStreaks, std::size(aBackdropStreaks));
		Graphics()->LinesEnd();
	}
	if(Animate && g_Config.m_ClZzVisualEffects && g_Config.m_ClZzMenuParticles &&
		g_Config.m_ClZzMenuParticleAmount > 0)
	{
		IGraphics::CQuadItem aAccentParticles[20];
		IGraphics::CQuadItem aAltParticles[20];
		int NumAccent = 0;
		int NumAlt = 0;
		const int ParticleCount = 6 + g_Config.m_ClZzMenuParticleAmount * 34 / 100;
		const float ParticleTime = LocalTime() * SpeedScale;
		for(int i = 0; i < ParticleCount; ++i)
		{
			const float SeedX = Hash01((uint32_t)i * 17U + 3U);
			const float SeedY = Hash01((uint32_t)i * 29U + 11U);
			const float Drift =
				sinf(ParticleTime * (0.45f + SeedY * 0.5f) + SeedX * 12.0f) *
				(8.0f + SeedY * 14.0f);
			const float X = SeedX * ScreenW + Drift;
			const float Y =
				fmodf(SeedY * ScreenH + ParticleTime * (9.0f + SeedX * 18.0f),
					ScreenH + 24.0f) -
				12.0f;
			const float Size = 1.2f + Hash01((uint32_t)i * 43U + 7U) * 2.6f;
			IGraphics::CQuadItem Item(X, Y, Size, Size);
			if((i & 1) == 0 && NumAccent < (int)std::size(aAccentParticles))
				aAccentParticles[NumAccent++] = Item;
			else if(NumAlt < (int)std::size(aAltParticles))
				aAltParticles[NumAlt++] = Item;
		}
		Graphics()->QuadsBegin();
		Graphics()->SetColor(Palette.m_Accent.WithAlpha(0.10f * Eased));
		Graphics()->QuadsDrawTL(aAccentParticles, NumAccent);
		Graphics()->SetColor(Palette.m_AccentAlt.WithAlpha(0.08f * Eased));
		Graphics()->QuadsDrawTL(aAltParticles, NumAlt);
		Graphics()->QuadsEnd();
	}

	int Order[NUM_PANELS];
	for(int Panel = 0; Panel < NUM_PANELS; ++Panel)
		Order[Panel] = Panel;
	std::sort(std::begin(Order), std::end(Order),
		[this](int A, int B) { return m_aZOrder[A] < m_aZOrder[B]; });
	for(int Index = 0; Index < NUM_PANELS; ++Index)
	{
		const int Panel = Order[Index];
		vec2 Pos = m_aPanelRenderPos[Panel];
		const float PanelPhase = Animate && Open ?
						 std::clamp((m_OpenElapsed - Panel * 0.050f) / 0.38f, 0.0f, 1.0f) :
						 Eased;
		const float PanelEase = Animate ?
			PanelPhase * PanelPhase * PanelPhase *
				(PanelPhase * (PanelPhase * 6.0f - 15.0f) + 10.0f) :
			PanelPhase;
		Pos.y += (1.0f - PanelEase) * 20.0f;
		Pos.x += (Panel < NUM_PANELS / 2 ? -1.0f : 1.0f) *
			 (1.0f - PanelEase) * 3.0f;
		m_aPanelVisualPos[Panel] = Pos;
		const float PanelAlpha =
			PanelEase * std::clamp(Eased * 1.5f, 0.0f, 1.0f);
		m_aPanelVisualAlpha[Panel] = PanelAlpha;
		RenderPanel(Panel, Pos, PanelAlpha);
	}
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
}
