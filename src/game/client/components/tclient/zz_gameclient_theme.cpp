#include <game/client/components/tclient/zz_theme.h>
#include <game/client/components/tclient/zz_theme_runtime.h>
#include <game/client/gameclient.h>

#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#include <generated/client_data.h>

void CGameClient::ApplyZZTheme()
{
	const ColorRGBA CustomAccent =
		color_cast<ColorRGBA>(ColorHSLA(g_Config.m_UiColor, true));
	const SZZThemePalette Palette =
		GetZZThemePalette(g_Config.m_ClZzTheme, CustomAccent);
	if(g_Config.m_ClZzTheme != 0)
		g_Config.m_UiColor = color_cast<ColorHSLA>(Palette.m_Accent).Pack(true);

	CImageInfo CursorImage;
	if(Graphics()->LoadPng(CursorImage,
		   g_pData->m_aImages[IMAGE_CURSOR].m_pFilename,
		   IStorage::TYPE_ALL) &&
		CursorImage.m_Format == CImageInfo::FORMAT_RGBA)
	{
		const ColorRGBA CursorAccent =
			ZZThemeLerp(Palette.m_Accent, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), 0.18f);
		ZzRecolorCursorPixels(CursorImage, 0, 0, CursorImage.m_Width,
			CursorImage.m_Height, CursorAccent);
		Graphics()->UnloadTexture(&g_pData->m_aImages[IMAGE_CURSOR].m_Id);
		g_pData->m_aImages[IMAGE_CURSOR].m_Id =
			Graphics()->LoadTextureRawMove(CursorImage, 0, "zz-theme-ui-cursor");
	}
	else
		CursorImage.Free();

	LoadGameSkin(g_Config.m_ClAssetGame);
}


