#ifndef GAME_CLIENT_ZZ_THEME_H
#define GAME_CLIENT_ZZ_THEME_H

#include <base/color.h>

#include <algorithm>

struct SZZThemePalette
{
	const char *m_pName;
	ColorRGBA m_Accent;
	ColorRGBA m_AccentAlt;
	ColorRGBA m_Background;
	ColorRGBA m_Panel;
	ColorRGBA m_PanelHover;
	ColorRGBA m_Text;
	ColorRGBA m_TextMuted;
};

inline ColorRGBA ZZThemeLerp(const ColorRGBA &A, const ColorRGBA &B, float Amount)
{
	return ColorRGBA(
		A.r + (B.r - A.r) * Amount,
		A.g + (B.g - A.g) * Amount,
		A.b + (B.b - A.b) * Amount,
		A.a + (B.a - A.a) * Amount);
}

inline SZZThemePalette GetZZThemePalette(int Theme, ColorRGBA CustomAccent = ColorRGBA(0.66f, 0.36f, 0.94f, 1.0f))
{
	switch(std::clamp(Theme, 0, 5))
	{
	case 0:
	{
		CustomAccent.a = 1.0f;
		const ColorRGBA AccentAlt = ZZThemeLerp(CustomAccent, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), 0.32f);
		return {"Custom", CustomAccent, AccentAlt, ColorRGBA(CustomAccent.r * 0.055f, CustomAccent.g * 0.055f, CustomAccent.b * 0.055f, 1.0f), ColorRGBA(CustomAccent.r * 0.13f, CustomAccent.g * 0.13f, CustomAccent.b * 0.13f, 1.0f), ColorRGBA(CustomAccent.r * 0.28f, CustomAccent.g * 0.28f, CustomAccent.b * 0.28f, 1.0f), ColorRGBA(0.96f, 0.95f, 0.98f, 1.0f), ColorRGBA(0.76f, 0.74f, 0.80f, 1.0f)};
	}
	case 2:
		return {"Ruby", ColorRGBA(0.94f, 0.22f, 0.38f, 1.0f), ColorRGBA(1.0f, 0.55f, 0.25f, 1.0f), ColorRGBA(0.065f, 0.018f, 0.027f, 1.0f), ColorRGBA(0.13f, 0.035f, 0.050f, 1.0f), ColorRGBA(0.27f, 0.075f, 0.10f, 1.0f), ColorRGBA(1.0f, 0.94f, 0.95f, 1.0f), ColorRGBA(0.84f, 0.72f, 0.74f, 1.0f)};
	case 3:
		return {"Emerald", ColorRGBA(0.15f, 0.80f, 0.55f, 1.0f), ColorRGBA(0.20f, 0.72f, 0.96f, 1.0f), ColorRGBA(0.012f, 0.052f, 0.041f, 1.0f), ColorRGBA(0.025f, 0.105f, 0.080f, 1.0f), ColorRGBA(0.055f, 0.23f, 0.17f, 1.0f), ColorRGBA(0.92f, 1.0f, 0.97f, 1.0f), ColorRGBA(0.68f, 0.84f, 0.77f, 1.0f)};
	case 4:
		return {"Cyber", ColorRGBA(0.10f, 0.78f, 0.98f, 1.0f), ColorRGBA(0.98f, 0.30f, 0.76f, 1.0f), ColorRGBA(0.010f, 0.038f, 0.060f, 1.0f), ColorRGBA(0.020f, 0.078f, 0.12f, 1.0f), ColorRGBA(0.045f, 0.17f, 0.25f, 1.0f), ColorRGBA(0.92f, 0.98f, 1.0f, 1.0f), ColorRGBA(0.66f, 0.80f, 0.86f, 1.0f)};
	case 5:
		return {"Mono", ColorRGBA(0.78f, 0.82f, 0.88f, 1.0f), ColorRGBA(0.50f, 0.56f, 0.66f, 1.0f), ColorRGBA(0.032f, 0.034f, 0.038f, 1.0f), ColorRGBA(0.075f, 0.080f, 0.090f, 1.0f), ColorRGBA(0.16f, 0.17f, 0.19f, 1.0f), ColorRGBA(0.97f, 0.97f, 0.98f, 1.0f), ColorRGBA(0.73f, 0.75f, 0.79f, 1.0f)};
	case 1:
	default:
		return {"Violet", ColorRGBA(0.66f, 0.36f, 0.94f, 1.0f), ColorRGBA(0.94f, 0.36f, 0.82f, 1.0f), ColorRGBA(0.040f, 0.022f, 0.057f, 1.0f), ColorRGBA(0.090f, 0.050f, 0.12f, 1.0f), ColorRGBA(0.20f, 0.11f, 0.27f, 1.0f), ColorRGBA(0.97f, 0.94f, 1.0f, 1.0f), ColorRGBA(0.79f, 0.72f, 0.84f, 1.0f)};
	}
}

#endif
