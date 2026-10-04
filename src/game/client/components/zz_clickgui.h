#ifndef GAME_CLIENT_COMPONENTS_ZZ_CLICKGUI_H
#define GAME_CLIENT_COMPONENTS_ZZ_CLICKGUI_H

#include <base/color.h>
#include <base/vmath.h>

#include <engine/shared/config.h>

#include <game/client/component.h>
#include <game/client/zz_theme.h>

class CZZClickGui : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }

	void OnReset() override;
	void OnRender() override;
	bool OnInput(const IInput::CEvent &Event) override;
	bool OnCursorMove(float x, float y, IInput::ECursorType CursorType) override;

private:
	static constexpr int NUM_PANELS = 6;
	// Utility currently has more than 18 rows. Keep enough room for all panels,
	// otherwise one panel's animation state overwrites the next array.
	static constexpr int MAX_PANEL_ROWS = 32;
	// Keep the six fixed cards compact. At the user's 1700px-wide resolution
	// this leaves the same side margins and density as the iOS 26 reference.
	static constexpr float PANEL_WIDTH = 250.0f;
	static constexpr float HEADER_HEIGHT = 38.0f;
	static constexpr float BODY_PADDING = 8.0f;
	static constexpr float ROW_GAP = 3.0f;

	vec2 m_aPanelPos[NUM_PANELS] = {};
	vec2 m_aPanelRenderPos[NUM_PANELS] = {};
	// Final on-screen positions include the opening animation. Keeping them
	// separately makes hit-testing match the pixels that are actually shown.
	vec2 m_aPanelVisualPos[NUM_PANELS] = {};
	float m_aPanelVisualAlpha[NUM_PANELS] = {};
	float m_aPanelScroll[NUM_PANELS] = {};
	float m_aPanelBodyViewportHeight[NUM_PANELS] = {};
	bool m_PositionsInitialized = false;
	bool m_aCollapsed[NUM_PANELS] = {};
	int m_aZOrder[NUM_PANELS] = {0, 1, 2, 3, 4, 5};
	vec2 m_MousePos = vec2(0.0f, 0.0f);
	float m_Anim = 0.0f;
	float m_OpenElapsed = 0.0f;
	bool m_WasOpen = false;
	bool m_OpenedOverMenu = false;
	float m_aCollapseAnim[NUM_PANELS] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
	float m_aPanelHoverAnim[NUM_PANELS] = {};
	float m_aaRowHoverAnim[NUM_PANELS][MAX_PANEL_ROWS] = {};
	float m_aaToggleAnim[NUM_PANELS][MAX_PANEL_ROWS] = {};
	float m_aaSliderAnim[NUM_PANELS][MAX_PANEL_ROWS] = {};
	float m_ThemePulse = 0.0f;
	float m_UiScale = 1.0f;
	int m_LastScreenWidth = 0;
	int m_LastScreenHeight = 0;
	int m_LastTheme = -1;
	SZZThemePalette m_RenderPalette = GetZZThemePalette(1);
	bool m_DraggingPanel = false;
	int m_DragPanel = -1;
	vec2 m_DragOffset = vec2(0.0f, 0.0f);
	int m_DragSliderPanel = -1;
	int m_DragSliderRow = -1;
	bool m_WaitingForBind = false;
	int CConfig::*m_pWaitingBind = nullptr;

	void InitializePositions(float ScreenW, float ScreenH);
	void BringToFront(int Panel);
	float VisiblePanelBodyHeight(int Panel, vec2 Pos) const;
	bool ScrollHoveredPanel(float Amount);
	void UpdateMousePosition();
	void CloseClickGui();
	void HandleMouseDown();
	void HandleMouseUp();
	void UpdateSlider(int Panel, int Row);
	void RenderPanel(int Panel, vec2 Pos, float Alpha);
	bool PointInRect(vec2 Point, float X, float Y, float W, float H) const;
	void DrawText(float X, float Y, float Size, const char *pText, const ColorRGBA &Color, bool RightAligned = false);
};

#endif
