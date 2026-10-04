#ifndef GAME_CLIENT_COMPONENTS_ZZ_VISUAL_EFFECTS_H
#define GAME_CLIENT_COMPONENTS_ZZ_VISUAL_EFFECTS_H

#include <base/color.h>
#include <base/vmath.h>

#include <game/client/component.h>

class CZZWorldPostProcess : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }
	void OnRender() override;
};

class CZZVisualEffects : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnRender() override;
	bool OnInput(const IInput::CEvent &Event) override;

private:
	static constexpr int CURSOR_TRAIL_POINTS = 48;
	static constexpr int CLICK_BURST_COUNT = 6;
	static constexpr int FEATURE_TOAST_COUNT = 5;
	struct SClickBurst
	{
		vec2 m_Pos = vec2(0.0f, 0.0f);
		float m_Age = -1.0f;
		float m_Rotation = 0.0f;
	};
	struct SFeatureToast
	{
		char m_aTitle[40] = {};
		float m_Age = -1.0f;
		bool m_Enabled = false;
	};

	bool m_PaletteInitialized = false;
	bool m_CursorTrailInitialized = false;
	vec2 m_aCursorTrail[CURSOR_TRAIL_POINTS] = {};
	SClickBurst m_aClickBursts[CLICK_BURST_COUNT] = {};
	int m_NextClickBurst = 0;
	float m_FreezeAmount = 0.0f;
	float m_FreezeImpact = 0.0f;
	float m_FreezePulseElapsed = 0.0f;
	bool m_WasFrozen = false;
	int m_LastVisualPreset = -1;
	bool m_FeatureWatchInitialized = false;
	int m_aFeatureWatch[5] = {};
	SFeatureToast m_aFeatureToasts[FEATURE_TOAST_COUNT] = {};
	int m_NextFeatureToast = 0;
	ColorRGBA m_Accent = ColorRGBA(0.66f, 0.36f, 0.94f, 1.0f);
	ColorRGBA m_AccentAlt = ColorRGBA(0.94f, 0.36f, 0.82f, 1.0f);
	void RenderMenuParticles(float ScreenW, float ScreenH, float Time,
		float Strength);
	void RenderCursorTrail(float ScreenW, float ScreenH, float Time,
		float Strength);
	void RenderClickBursts(float ScreenW, float ScreenH, float Strength);
	void ApplyVisualPreset();
	void UpdateFeatureToasts(float DeltaTime);
	void RenderAmbientOverlay(float ScreenW, float ScreenH, float Time,
		float Strength);
	void RenderInterfaceHud(float ScreenW, float ScreenH, float Strength);
};

#endif
