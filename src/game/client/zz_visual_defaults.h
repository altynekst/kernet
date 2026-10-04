#ifndef GAME_CLIENT_ZZ_VISUAL_DEFAULTS_H
#define GAME_CLIENT_ZZ_VISUAL_DEFAULTS_H

#include <engine/shared/config.h>

// Return the renderer to the ordinary DDNet presentation while leaving gameplay
// helpers and the ClickGUI itself untouched.
inline void ApplyZZVanillaVisuals()
{
	g_Config.m_ClZzAspectRatio = 0;
	g_Config.m_ClZzVisualEffects = 0;
	g_Config.m_ClZzClickGuiBackdropEffects = 0;
	g_Config.m_ClZzPostProcessing = 0;
	g_Config.m_ClZzVisualEffectsMenu = 0;
	g_Config.m_ClZzMenuParticles = 0;
	g_Config.m_ClZzCursorTrail = 0;
	g_Config.m_ClZzClickBursts = 0;
	g_Config.m_ClZzChatBubbles = 0;
	g_Config.m_ClZzVisualEffectsGameplay = 0;
	g_Config.m_ClZzVisualEffectsFreeze = 0;
	g_Config.m_ClZzVisualEffectsFngPulse = 0;
	g_Config.m_ClZzVisualPreset = 0;
	g_Config.m_ClZzVisualAdaptive = 0;
	g_Config.m_ClZzVisualFeatureToasts = 0;
	g_Config.m_ClZzVisualAmbientSweep = 0;
	g_Config.m_ClZzVisualFocusBrackets = 0;
	g_Config.m_ClZzVisualWorldAtmosphere = 0;
	g_Config.m_ClZzVisualPlayerMotion = 0;
	g_Config.m_ClZzVisualWeaponEffects = 0;
	g_Config.m_ClZzVisualOtherTrails = 0;
	g_Config.m_ClZzVisualDynamicLights = 0;
	g_Config.m_ClZzVisualPlayerPresence = 0;
}

#endif
