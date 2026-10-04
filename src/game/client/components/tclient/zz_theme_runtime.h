#pragma once

#include <base/color.h>
#include <base/math.h>

#include <engine/image.h>

inline void ZzRecolorCursorPixels(CImageInfo &Image, size_t X, size_t Y, size_t Width,
	size_t Height, const ColorRGBA &Accent)
{
	if(Image.m_Format != CImageInfo::FORMAT_RGBA || !Image.m_pData)
		return;

	const size_t EndX = minimum(X + Width, Image.m_Width);
	const size_t EndY = minimum(Y + Height, Image.m_Height);
	for(size_t Py = Y; Py < EndY; ++Py)
	{
		for(size_t Px = X; Px < EndX; ++Px)
		{
			uint8_t *pPixel = Image.m_pData + (Py * Image.m_Width + Px) * 4;
			const int Value =
				maximum((int)pPixel[0], maximum((int)pPixel[1], (int)pPixel[2]));
			if(pPixel[3] == 0 || Value <= 52)
				continue;

			const float Shade = 0.45f + 0.55f * ((float)Value / 255.0f);
			pPixel[0] = (uint8_t)round_to_int(255.0f * Accent.r * Shade);
			pPixel[1] = (uint8_t)round_to_int(255.0f * Accent.g * Shade);
			pPixel[2] = (uint8_t)round_to_int(255.0f * Accent.b * Shade);
		}
	}
}

inline void ZzRecolorGameSkinCursors(CImageInfo &Image, const ColorRGBA &Accent)
{
	if(Image.m_Format != CImageInfo::FORMAT_RGBA || Image.m_Width % 32 != 0 ||
		Image.m_Height % 16 != 0)
		return;

	const size_t CellWidth = Image.m_Width / 32;
	const size_t CellHeight = Image.m_Height / 16;
	static constexpr int s_aCursorRows[] = {0, 4, 6, 8, 10, 12};
	for(const int Row : s_aCursorRows)
		ZzRecolorCursorPixels(Image, 0, Row * CellHeight, CellWidth * 2,
			CellHeight * 2, Accent);
}


