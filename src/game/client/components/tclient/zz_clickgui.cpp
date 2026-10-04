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
#include "zz_theme.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace
{
	enum class ERowType
	{
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
		bool m_Experimental = false;
	};

	constexpr float RowHeight(ERowType Type)
	{
		return Type == ERowType::SLIDER ? 49.0f : 31.0f;
	}

	static const char *const s_apAimModes[] = {"Light", "Medium", "Strong"};
	static const char *const s_apThemes[] = {"Custom", "Violet", "Ruby",
		"Emerald", "Cyber", "Mono"};

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
		{ERowType::TOGGLE, "Pilot Bot Fly", &CConfig::m_ClZzFlyRide, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Pilot freeze assist (aled)", &CConfig::m_ClZzFlyRideFreezeAssist,
			0, 1, 1, nullptr, 0},
		{ERowType::TOGGLE, "Fly", &CConfig::m_ClZzFlyHelperEnabled, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Hook fly", &CConfig::m_ClZzHookFly, 0, 1, 1, nullptr,
			0},
		{ERowType::TOGGLE, "Triple fly", &CConfig::m_ClZzTripleFly, 0, 1, 1,
			nullptr, 0},
		{ERowType::SLIDER, "Hit range x0.1", &CConfig::m_ClZzFlyHelperHitTiles, 1,
			80, 1, nullptr, 0},
		{ERowType::SLIDER, "Hit delay ms", &CConfig::m_ClZzFlyHelperHitMs, 0, 1000,
			10, nullptr, 0},
		{ERowType::TOGGLE, "Relaxed server", &CConfig::m_ClZzFlyHelperRelaxed, 0, 1,
			1, nullptr, 0},
		{ERowType::TOGGLE, "Balance bot", &CConfig::m_ClZzBalanceBot, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Avoid freeze", &CConfig::m_ClZzAvoidEnabled, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Avoid tele", &CConfig::m_ClZzAvoidTele, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Avoid kill", &CConfig::m_ClZzAvoidKill, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Hook assist", &CConfig::m_ClZzAvoidHookAssist, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Avoid jump", &CConfig::m_ClZzAvoidJump, 0, 1, 1,
			nullptr, 0},
		{ERowType::SLIDER, "Lookahead", &CConfig::m_ClZzAvoidLookahead, 4, 128, 4,
			nullptr, 0},
		{ERowType::SLIDER, "Hook range", &CConfig::m_ClZzAvoidHookAssistRange, 16,
			512, 16, nullptr, 0},
	};

	static SRow s_aUtilityRows[] = {
		{ERowType::TOGGLE, "Auto unfreeze", &CConfig::m_ClZzAutoUnfreeze, 0, 1, 1,
			nullptr, 0},
		{ERowType::TOGGLE, "Unfreeze: auto laser",
			&CConfig::m_ClZzAutoUnfreezeAutoLaser, 0, 1, 1, nullptr, 0},
		{ERowType::SLIDER, "Unfreeze FOV", &CConfig::m_ClZzAutoUnfreezeFov, 5,
			360, 5, nullptr, 0},
		{ERowType::SLIDER, "Unfreeze angles", &CConfig::m_ClZzAutoUnfreezeAngles,
			1, 144, 1, nullptr, 0},
		{ERowType::SLIDER, "Unfreeze ticks", &CConfig::m_ClZzAutoUnfreezeTicks, 1,
			50, 1, nullptr, 0},
		{ERowType::SLIDER, "Unfreeze trigger ticks",
			&CConfig::m_ClZzAutoUnfreezeTriggerTicks, 1, 5, 1, nullptr, 0},
		{ERowType::TOGGLE, "Unfreeze: silent",
			&CConfig::m_ClZzAutoUnfreezeSilent, 0, 1, 1, nullptr, 0},
		{ERowType::TOGGLE, "Unfreeze: show attempt",
			&CConfig::m_ClZzAutoUnfreezeShowAttempt, 0, 1, 1, nullptr, 0},
		{ERowType::MODE, "Theme", &CConfig::m_ClZzTheme, 0, 5, 1, s_apThemes, 6},
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
		{ERowType::KEYBIND, "Open key", &CConfig::m_ClZzClickGuiKey, 0, 512, 1,
			nullptr, 0},
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
	};

	struct SPanel
	{
		const char *m_pTitle;
		SRow *m_pRows;
		int m_NumRows;
	};

	static SPanel s_aPanels[] = {
		{"AIM", s_aAimRows, (int)std::size(s_aAimRows)},
		{"FNG LASER", s_aFngRows, (int)std::size(s_aFngRows)},
		{"MOVEMENT", s_aMovementRows, (int)std::size(s_aMovementRows)},
		{"UTILITY", s_aUtilityRows, (int)std::size(s_aUtilityRows)},
	};

	float PanelBodyHeight(int Panel)
	{
		float Height = 0.0f;
		for(int i = 0; i < s_aPanels[Panel].m_NumRows; ++i)
			Height += RowHeight(s_aPanels[Panel].m_pRows[i].m_Type) + 4.0f;
		return Height + 10.0f;
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
	m_WasOpen = false;
	m_PositionsInitialized = false;
	m_DraggingPanel = false;
	m_DragPanel = -1;
	m_DragSliderPanel = -1;
	m_DragSliderRow = -1;
	m_WaitingForBind = false;
	m_ThemePulse = 0.0f;
	m_LastTheme = g_Config.m_ClZzTheme;
	m_RenderPalette = GetZZThemePalette(
		g_Config.m_ClZzTheme,
		color_cast<ColorRGBA>(ColorHSLA(g_Config.m_UiColor, true)));
	for(int i = 0; i < NUM_PANELS; ++i)
	{
		m_aCollapsed[i] = false;
		m_aZOrder[i] = i;
		m_aCollapseAnim[i] = 1.0f;
		m_aPanelScroll[i] = 0.0f;
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
	const float Gap = 18.0f;
	const bool TwoRows = ScreenW < PANEL_WIDTH * 4.0f + Gap * 5.0f;
	const int Columns = TwoRows ? 2 : 4;
	const float TotalWidth = Columns * PANEL_WIDTH + (Columns - 1) * Gap;
	const float StartX = std::max(18.0f, (ScreenW - TotalWidth) * 0.5f);
	for(int i = 0; i < NUM_PANELS; ++i)
	{
		const int Column = i % Columns;
		const int Row = i / Columns;
		m_aPanelPos[i] = vec2(StartX + Column * (PANEL_WIDTH + Gap),
			72.0f + Row * std::min(470.0f, ScreenH * 0.48f));
		m_aPanelRenderPos[i] = m_aPanelPos[i];
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
	m_aCollapseAnim[Panel] =
		Approach(m_aCollapseAnim[Panel], m_aCollapsed[Panel] ? 0.0f : 1.0f);
	const float FullBodyHeight = PanelBodyHeight(Panel);
	const float VisibleFullBodyHeight = std::min(FullBodyHeight,
		std::max(0.0f, (float)Graphics()->ScreenHeight() - Pos.y - HEADER_HEIGHT - 12.0f));
	const float MaxScroll = std::max(0.0f, FullBodyHeight - VisibleFullBodyHeight);
	m_aPanelScroll[Panel] = std::clamp(m_aPanelScroll[Panel], 0.0f, MaxScroll);
	const float BodyHeight = VisibleFullBodyHeight * m_aCollapseAnim[Panel];
	const bool PanelHovered = PointInRect(m_MousePos, Pos.x, Pos.y, PANEL_WIDTH,
		HEADER_HEIGHT + BodyHeight);
	m_aPanelHoverAnim[Panel] =
		Approach(m_aPanelHoverAnim[Panel], PanelHovered ? 1.0f : 0.0f);
	const float HoverGlow = m_aPanelHoverAnim[Panel];
	const ColorRGBA Border =
		Palette.m_Accent.WithAlpha((0.55f + HoverGlow * 0.35f) * Alpha);
	const ColorRGBA Background = Palette.m_Background.WithAlpha(0.97f * Alpha);
	const ColorRGBA Header = ZZThemeLerp(Palette.m_Panel, Palette.m_Accent,
		0.72f + 0.10f * m_ThemePulse)
					 .WithAlpha(Alpha);
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

	const bool Dragged = m_DraggingPanel && m_DragPanel == Panel;
	Graphics()->DrawRect(
		Pos.x + (Dragged ? 8.0f : 4.0f), Pos.y + (Dragged ? 10.0f : 6.0f),
		PANEL_WIDTH, HEADER_HEIGHT + BodyHeight,
		ColorRGBA(0.0f, 0.0f, 0.0f, (0.24f + HoverGlow * 0.10f) * Alpha),
		IGraphics::CORNER_ALL, 7.0f);
	Graphics()->DrawRect(Pos.x - 1.0f, Pos.y - 1.0f, PANEL_WIDTH + 2.0f,
		HEADER_HEIGHT + BodyHeight + 2.0f, Border,
		IGraphics::CORNER_ALL, 7.0f);
	Graphics()->DrawRect(Pos.x, Pos.y, PANEL_WIDTH, HEADER_HEIGHT + BodyHeight,
		Background, IGraphics::CORNER_ALL, 6.0f);
	Graphics()->DrawRect(Pos.x, Pos.y, PANEL_WIDTH, HEADER_HEIGHT, Header,
		IGraphics::CORNER_T, 6.0f);
	if(Animate)
	{
		Graphics()->ClipEnable((int)Pos.x, (int)Pos.y, (int)PANEL_WIDTH,
			(int)HEADER_HEIGHT);
		const float SweepW = 54.0f;
		const float SweepX =
			Pos.x - SweepW +
			fmodf(LocalTime() * (90.0f + g_Config.m_ClZzAnimationSpeed) +
					Panel * 71.0f,
				PANEL_WIDTH + SweepW * 2.0f);
		Graphics()->DrawRect(SweepX, Pos.y + HEADER_HEIGHT - 2.0f, SweepW, 2.0f,
			Palette.m_AccentAlt.WithAlpha(0.65f * Alpha),
			IGraphics::CORNER_ALL, 1.0f);
		Graphics()->ClipDisable();
	}
	DrawText(Pos.x + 13.0f + HoverGlow * 2.0f, Pos.y + 9.0f, 18.0f,
		PanelData.m_pTitle, Palette.m_Text.WithAlpha(Alpha));
	DrawText(Pos.x + PANEL_WIDTH - 13.0f, Pos.y + 9.0f, 18.0f,
		m_aCollapsed[Panel] ? "+" : "-", Palette.m_Text.WithAlpha(Alpha),
		true);

	if(m_aCollapseAnim[Panel] <= 0.01f)
		return;

	Graphics()->ClipEnable((int)Pos.x, (int)(Pos.y + HEADER_HEIGHT),
		(int)PANEL_WIDTH, (int)(BodyHeight + 1.0f));
	float Y = Pos.y + HEADER_HEIGHT + BODY_PADDING - m_aPanelScroll[Panel];
	for(int RowIndex = 0; RowIndex < PanelData.m_NumRows; ++RowIndex)
	{
		const SRow &Row = PanelData.m_pRows[RowIndex];
		int &Value = g_Config.*(Row.m_pValue);
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
			Graphics()->DrawRect(
				Pos.x + BODY_PADDING - Hover * 2.0f, DrawY,
				PANEL_WIDTH - BODY_PADDING * 2.0f + Hover * 4.0f, Height,
				Palette.m_PanelHover.WithAlpha(0.78f * Hover * RowAlpha),
				IGraphics::CORNER_ALL, 4.0f);

		if(Row.m_Type == ERowType::TOGGLE)
		{
			const float BoxX = Pos.x + PANEL_WIDTH - BODY_PADDING - 23.0f;
			if(Row.m_Experimental)
			{
				const float BadgeW = 53.0f;
				const float BadgeH = 17.0f;
				const float BadgeX = BoxX - BadgeW - 7.0f;
				const float BadgeY = DrawY + 6.5f;
				Graphics()->DrawRect(BadgeX, BadgeY, BadgeW, BadgeH,
					Palette.m_Accent.WithAlpha(0.20f * RowAlpha),
					IGraphics::CORNER_ALL, 4.0f);
				Graphics()->DrawRect(BadgeX, BadgeY, 17.0f, BadgeH,
					Palette.m_Accent.WithAlpha(0.82f * RowAlpha),
					IGraphics::CORNER_L, 4.0f);
				DrawCenteredIcon(BadgeX, BadgeY, 17.0f, BadgeH, 10.0f, FontIcon::FLASK,
					Palette.m_Text.WithAlpha(RowAlpha));
				DrawText(BadgeX + 21.0f, BadgeY + 2.0f, 10.0f, "EXP",
					Palette.m_AccentAlt.WithAlpha(RowAlpha));
			}
			m_aaToggleAnim[Panel][RowIndex] =
				Approach(m_aaToggleAnim[Panel][RowIndex], Value ? 1.0f : 0.0f);
			const float Toggle = m_aaToggleAnim[Panel][RowIndex];
			Graphics()->DrawRect(BoxX, DrawY + 6.0f, 18.0f, 18.0f,
				Palette.m_Panel.WithAlpha(RowAlpha),
				IGraphics::CORNER_ALL, 4.0f);
			if(Toggle > 0.001f)
				Graphics()->DrawRect(BoxX + 9.0f * (1.0f - Toggle),
					DrawY + 6.0f + 9.0f * (1.0f - Toggle),
					18.0f * Toggle, 18.0f * Toggle,
					Palette.m_Accent.WithAlpha(RowAlpha),
					IGraphics::CORNER_ALL, 4.0f);
			if(Toggle > 0.55f)
				DrawCenteredIcon(
					BoxX, DrawY + 6.0f, 18.0f, 18.0f, 11.5f, FontIcon::XMARK,
					Palette.m_Text.WithAlpha((Toggle - 0.55f) / 0.45f * RowAlpha));
			DrawText(Pos.x + BODY_PADDING + 5.0f + Hover * 2.0f, DrawY + 6.0f, 16.0f,
				Row.m_pName, Palette.m_Text.WithAlpha(RowAlpha));
		}
		else if(Row.m_Type == ERowType::MODE)
		{
			DrawText(Pos.x + BODY_PADDING + 5.0f + Hover * 2.0f, DrawY + 6.0f, 16.0f,
				Row.m_pName, Palette.m_Text.WithAlpha(RowAlpha));
			const int Option = std::clamp(Value, 0, Row.m_NumOptions - 1);
			DrawText(Pos.x + PANEL_WIDTH - BODY_PADDING - 7.0f, DrawY + 6.0f, 16.0f,
				Row.m_ppOptions[Option], Palette.m_AccentAlt.WithAlpha(RowAlpha),
				true);
		}
		else if(Row.m_Type == ERowType::KEYBIND)
		{
			DrawText(Pos.x + BODY_PADDING + 5.0f + Hover * 2.0f, DrawY + 6.0f, 16.0f,
				Row.m_pName, Palette.m_Text.WithAlpha(RowAlpha));
			const char *pKeyName =
				m_WaitingForBind ? "Press a key..." : Input()->KeyName(Value);
			DrawText(Pos.x + PANEL_WIDTH - BODY_PADDING - 7.0f, DrawY + 6.0f, 16.0f,
				pKeyName, Palette.m_AccentAlt.WithAlpha(RowAlpha), true);
		}
		else
		{
			char aValue[32];
			str_format(aValue, sizeof(aValue), "%d", Value);
			DrawText(Pos.x + BODY_PADDING + 5.0f + Hover * 2.0f, DrawY + 3.0f, 15.0f,
				Row.m_pName, Palette.m_Text.WithAlpha(RowAlpha));
			DrawText(Pos.x + PANEL_WIDTH - BODY_PADDING - 6.0f, DrawY + 3.0f, 15.0f,
				aValue, Palette.m_AccentAlt.WithAlpha(RowAlpha), true);
			const float TrackX = Pos.x + BODY_PADDING + 5.0f;
			const float TrackY = DrawY + 31.0f;
			const float TrackW = PANEL_WIDTH - BODY_PADDING * 2.0f - 10.0f;
			const float TargetPercent =
				Row.m_Max == Row.m_Min ? 0.0f : (float)(Value - Row.m_Min) / (float)(Row.m_Max - Row.m_Min);
			m_aaSliderAnim[Panel][RowIndex] =
				Approach(m_aaSliderAnim[Panel][RowIndex], TargetPercent);
			const float Percent =
				std::clamp(m_aaSliderAnim[Panel][RowIndex], 0.0f, 1.0f);
			Graphics()->DrawRect(TrackX, TrackY, TrackW, 5.0f,
				Palette.m_Panel.WithAlpha(RowAlpha),
				IGraphics::CORNER_ALL, 2.5f);
			Graphics()->DrawRect(TrackX, TrackY, TrackW * Percent, 5.0f,
				Palette.m_Accent.WithAlpha(RowAlpha),
				IGraphics::CORNER_ALL, 2.5f);
			const float KnobPulse =
				Animate ? Hover * (1.0f + 0.5f * sinf(LocalTime() * 8.0f + RowIndex)) : 0.0f;
			Graphics()->DrawRect(TrackX + TrackW * Percent - 4.0f - KnobPulse,
				TrackY - 3.0f - KnobPulse, 8.0f + KnobPulse * 2.0f,
				11.0f + KnobPulse * 2.0f,
				Palette.m_AccentAlt.WithAlpha(RowAlpha),
				IGraphics::CORNER_ALL, 4.0f + KnobPulse);
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
	const float TrackX = m_aPanelRenderPos[Panel].x + BODY_PADDING + 5.0f;
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
	int Order[NUM_PANELS] = {0, 1, 2, 3};
	std::sort(std::begin(Order), std::end(Order),
		[this](int A, int B) { return m_aZOrder[A] > m_aZOrder[B]; });
	for(int Index = 0; Index < NUM_PANELS; ++Index)
	{
		const int Panel = Order[Index];
		const vec2 Pos = m_aPanelRenderPos[Panel];
		if(PointInRect(m_MousePos, Pos.x, Pos.y, PANEL_WIDTH, HEADER_HEIGHT))
		{
			BringToFront(Panel);
			if(m_MousePos.x >= Pos.x + PANEL_WIDTH - 38.0f)
				m_aCollapsed[Panel] = !m_aCollapsed[Panel];
			else
			{
				m_DraggingPanel = true;
				m_DragPanel = Panel;
				m_DragOffset = m_MousePos - Pos;
			}
			return;
		}
		if(m_aCollapsed[Panel])
			continue;
		const float FullBodyHeight = PanelBodyHeight(Panel);
		const float VisibleBodyHeight = std::min(FullBodyHeight,
			std::max(0.0f, (float)Graphics()->ScreenHeight() - Pos.y - HEADER_HEIGHT - 12.0f));
		if(!PointInRect(m_MousePos, Pos.x, Pos.y + HEADER_HEIGHT, PANEL_WIDTH,
			VisibleBodyHeight))
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
					m_WaitingForBind = true;
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
	// The click GUI is ahead of chat in the input stack, so text keys must pass
	// through untouched while chat or the console owns keyboard input.
	if((GameClient()->m_Chat.IsActive() ||
		   GameClient()->m_GameConsole.IsActive()) &&
		!m_WaitingForBind)
		return false;

	if(g_Config.m_ClZzClickGui && m_WaitingForBind &&
		(Event.m_Flags & IInput::FLAG_PRESS) &&
		(Event.m_Flags & IInput::FLAG_REPEAT) == 0 && Event.m_Key != KEY_MOUSE_1)
	{
		m_WaitingForBind = false;
		if(Event.m_Key == KEY_ESCAPE)
		{
			// Escape only cancels key capture. It must not also close ClickGUI
			// and open the pause menu behind it.
			return true;
		}
		if(Event.m_Key >= KEY_FIRST && Event.m_Key < KEY_LAST)
		{
			g_Config.m_ClZzClickGuiKey = Event.m_Key;
			g_Config.m_ClZzClickGuiMod = CBinds::GetModifierMask(Input()) &
						     ~CBinds::GetModifierMaskOfKey(Event.m_Key);
		}
		return true;
	}

	const int EventModifierMask = CBinds::GetModifierMask(Input()) &
				      ~CBinds::GetModifierMaskOfKey(Event.m_Key);
	const bool ConfiguredToggleKey =
		g_Config.m_ClZzClickGuiKey != KEY_UNKNOWN &&
		Event.m_Key == g_Config.m_ClZzClickGuiKey &&
		EventModifierMask == g_Config.m_ClZzClickGuiMod;
	// Insert is a permanent fallback so an old or accidentally changed config
	// can never make ClickGUI unreachable.
	const bool InsertFallback = Event.m_Key == KEY_INSERT && EventModifierMask == 0;
	if((Event.m_Flags & IInput::FLAG_PRESS) &&
		(Event.m_Flags & IInput::FLAG_REPEAT) == 0 &&
		(ConfiguredToggleKey || InsertFallback))
	{
		// A regular bind is processed before this component. If that bind already
		// changed the setting, do not flip it a second time on the same key press.
		if((g_Config.m_ClZzClickGui != 0) == m_WasOpen)
			g_Config.m_ClZzClickGui ^= 1;
		m_WaitingForBind = false;
		if(!g_Config.m_ClZzClickGui && !GameClient()->m_GameConsole.IsActive())
			Input()->MouseModeRelative();
		return true;
	}
	if(!g_Config.m_ClZzClickGui)
		return false;
	if((Event.m_Flags & IInput::FLAG_PRESS) && Event.m_Key == KEY_ESCAPE)
	{
		g_Config.m_ClZzClickGui = 0;
		m_WaitingForBind = false;
		m_Anim = 0.0f;
		HandleMouseUp();
		if(!GameClient()->m_GameConsole.IsActive() && !GameClient()->m_Menus.IsActive())
			Input()->MouseModeRelative();
		return true;
	}
	if((Event.m_Flags & IInput::FLAG_PRESS) && Event.m_Key == KEY_MOUSE_1)
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
	return g_Config.m_ClZzClickGui != 0;
}

void CZZClickGui::OnRender()
{
	const bool Open = g_Config.m_ClZzClickGui != 0;
	if(!Open && m_WasOpen && !GameClient()->m_GameConsole.IsActive() &&
		!GameClient()->m_Menus.IsActive())
		Input()->MouseModeRelative();
	m_WasOpen = Open;
	// Closing must be immediate. A close animation left the last (usually AIM)
	// panel visible as a translucent ghost for a few seconds.
	if(!Open)
	{
		m_Anim = 0.0f;
		HandleMouseUp();
		return;
	}
	const bool Animate = g_Config.m_ClZzClickGuiAnimations != 0;
	const float DeltaTime = std::clamp(Client()->RenderFrameTime(), 0.0f, 0.05f);
	const float SpeedScale = g_Config.m_ClZzAnimationSpeed / 100.0f;
	const float TargetAnim = Open ? 1.0f : 0.0f;
	m_Anim = Animate ? SmoothValue(m_Anim, TargetAnim, DeltaTime, 14.0f * SpeedScale) : TargetAnim;
	if(std::abs(m_Anim - TargetAnim) < 0.001f)
		m_Anim = TargetAnim;
	if(m_Anim <= 0.0f && !Open)
	{
		if(!GameClient()->m_GameConsole.IsActive() &&
			!GameClient()->m_Menus.IsActive())
			Input()->MouseModeRelative();
		return;
	}

	m_MousePos = Input()->NativeMousePos();
	const float ScreenW = (float)Graphics()->ScreenWidth();
	const float ScreenH = (float)Graphics()->ScreenHeight();
	if(!m_PositionsInitialized)
		InitializePositions(ScreenW, ScreenH);
	if(Open && !GameClient()->m_GameConsole.IsActive())
		Input()->MouseModeAbsolute();

	if(m_DraggingPanel && m_DragPanel >= 0)
	{
		vec2 NewPos = m_MousePos - m_DragOffset;
		NewPos.x = std::clamp(NewPos.x, -PANEL_WIDTH + 55.0f, ScreenW - 55.0f);
		NewPos.y = std::clamp(NewPos.y, 0.0f, ScreenH - HEADER_HEIGHT);
		m_aPanelPos[m_DragPanel] = NewPos;
	}
	for(int Panel = 0; Panel < NUM_PANELS; ++Panel)
	{
		const float DragResponse = Panel == m_DragPanel ? 28.0f : 18.0f;
		m_aPanelRenderPos[Panel] =
			Animate ? SmoothPosition(m_aPanelRenderPos[Panel], m_aPanelPos[Panel],
					  DeltaTime, DragResponse * SpeedScale) :
				  m_aPanelPos[Panel];
	}
	if(m_DragSliderPanel >= 0 && m_DragSliderRow >= 0)
		UpdateSlider(m_DragSliderPanel, m_DragSliderRow);
	if(!m_DraggingPanel && !m_WaitingForBind &&
		(Input()->KeyPress(KEY_MOUSE_WHEEL_UP) || Input()->KeyPress(KEY_MOUSE_WHEEL_DOWN)))
	{
		int Order[NUM_PANELS] = {0, 1, 2, 3};
		std::sort(std::begin(Order), std::end(Order),
			[this](int A, int B) { return m_aZOrder[A] > m_aZOrder[B]; });
		for(int Index = 0; Index < NUM_PANELS; ++Index)
		{
			const int Panel = Order[Index];
			if(m_aCollapsed[Panel])
				continue;
			const vec2 Pos = m_aPanelRenderPos[Panel];
			const float FullBodyHeight = PanelBodyHeight(Panel);
			const float VisibleBodyHeight = std::min(FullBodyHeight,
				std::max(0.0f, ScreenH - Pos.y - HEADER_HEIGHT - 12.0f));
			if(!PointInRect(m_MousePos, Pos.x, Pos.y + HEADER_HEIGHT, PANEL_WIDTH,
				VisibleBodyHeight))
				continue;
			const float MaxScroll = std::max(0.0f, FullBodyHeight - VisibleBodyHeight);
			const float ScrollDelta = Input()->KeyPress(KEY_MOUSE_WHEEL_UP) ? -48.0f : 48.0f;
			m_aPanelScroll[Panel] = std::clamp(
				m_aPanelScroll[Panel] + ScrollDelta, 0.0f, MaxScroll);
			break;
		}
	}

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
	const float Eased =
		Animate ? m_Anim * m_Anim * (3.0f - 2.0f * m_Anim) : m_Anim;
	Graphics()->MapScreen(0.0f, 0.0f, ScreenW, ScreenH);
	Graphics()->TextureClear();
	const float BackdropSoftness =
		std::clamp(g_Config.m_ClZzClickGuiBackdropSoftness / 100.0f, 0.0f, 1.0f);
	const ColorRGBA BackdropTint = ZZThemeLerp(ColorRGBA(0.0f, 0.0f, 0.0f, 1.0f),
		Palette.m_Background, 0.72f);
	Graphics()->DrawRect(
		0.0f, 0.0f, ScreenW, ScreenH,
		BackdropTint.WithAlpha((0.18f + BackdropSoftness * 0.22f) * Eased),
		IGraphics::CORNER_NONE, 0.0f);
	if(g_Config.m_ClZzClickGuiBackdropEffects && g_Config.m_ClZzVisualEffects)
	{
		const float EffectStrength =
			BackdropSoftness * Eased *
			std::clamp(g_Config.m_ClZzVisualEffectsIntensity / 100.0f, 0.0f, 1.0f);
		for(int Pass = 0; Pass < 4; ++Pass)
		{
			const float Inset = Pass * 18.0f;
			const float Edge = 26.0f + Pass * 9.0f;
			const float Alpha = (0.018f + Pass * 0.006f) * EffectStrength;
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
		Graphics()->SetColor(Palette.m_Accent.WithAlpha(0.018f * EffectStrength));
		Graphics()->QuadsDrawFreeform(aAccentBands, std::size(aAccentBands));
		Graphics()->SetColor(
			Palette.m_AccentAlt.WithAlpha(0.014f * EffectStrength));
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
			Palette.m_AccentAlt.WithAlpha(0.075f * EffectStrength));
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
		Graphics()->SetColor(Palette.m_Accent.WithAlpha(0.20f * Eased));
		Graphics()->QuadsDrawTL(aAccentParticles, NumAccent);
		Graphics()->SetColor(Palette.m_AccentAlt.WithAlpha(0.16f * Eased));
		Graphics()->QuadsDrawTL(aAltParticles, NumAlt);
		Graphics()->QuadsEnd();
	}
	if(Animate)
	{
		const float Pulse = 0.5f + 0.5f * sinf(LocalTime() * 2.2f);
		Graphics()->DrawRect(
			0.0f, 0.0f, ScreenW, 2.0f + m_ThemePulse * 3.0f,
			Palette.m_Accent.WithAlpha(
				(0.20f + Pulse * 0.08f + m_ThemePulse * 0.35f) * Eased),
			IGraphics::CORNER_NONE, 0.0f);
		Graphics()->DrawRect(
			0.0f, ScreenH - 2.0f - m_ThemePulse * 3.0f, ScreenW,
			2.0f + m_ThemePulse * 3.0f,
			Palette.m_AccentAlt.WithAlpha(
				(0.16f + Pulse * 0.06f + m_ThemePulse * 0.28f) * Eased),
			IGraphics::CORNER_NONE, 0.0f);
	}

	int Order[NUM_PANELS] = {0, 1, 2, 3};
	std::sort(std::begin(Order), std::end(Order),
		[this](int A, int B) { return m_aZOrder[A] < m_aZOrder[B]; });
	for(int Index = 0; Index < NUM_PANELS; ++Index)
	{
		const int Panel = Order[Index];
		vec2 Pos = m_aPanelRenderPos[Panel];
		const float PanelPhase =
			Animate ? std::clamp(Eased * 1.32f - Index * 0.08f, 0.0f, 1.0f) : Eased;
		const float PanelEase =
			Animate ? 1.0f - std::pow(1.0f - PanelPhase, 3.0f) : PanelPhase;
		Pos.y += (1.0f - PanelEase) * (-90.0f - Panel * 20.0f);
		Pos.x += (1.0f - PanelEase) * (Panel % 2 == 0 ? -55.0f : 55.0f);
		RenderPanel(Panel, Pos, PanelEase);
	}
	RenderTools()->RenderCursor(m_MousePos, 24.0f);
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
}
