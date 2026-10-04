#ifndef GAME_CLIENT_CHAT_BUBBLE_H
#define GAME_CLIENT_CHAT_BUBBLE_H

#include <base/str.h>

#include <generated/protocol.h>

constexpr bool ShouldShowWorldChatBubble(int Team)
{
	return Team == 0 || Team == 1;
}

static_assert(ShouldShowWorldChatBubble(0));
static_assert(ShouldShowWorldChatBubble(1));
static_assert(!ShouldShowWorldChatBubble(TEAM_WHISPER_SEND));
static_assert(!ShouldShowWorldChatBubble(TEAM_WHISPER_RECV));

inline int ChatBubbleRevealBytes(const char *pText, int VisibleCharacters)
{
	if(!pText || VisibleCharacters <= 0)
		return 0;

	int Cursor = 0;
	for(int Character = 0; pText[Cursor] != '\0' && Character < VisibleCharacters;
		Character++)
	{
		const int NextCursor = str_utf8_forward(pText, Cursor);
		if(NextCursor <= Cursor)
			return Cursor + 1;
		Cursor = NextCursor;
	}
	return Cursor;
}

inline int ChatBubbleCharacterCount(const char *pText)
{
	if(!pText)
		return 0;

	int Characters = 0;
	int Cursor = 0;
	while(pText[Cursor] != '\0')
	{
		const int NextCursor = str_utf8_forward(pText, Cursor);
		Cursor = NextCursor > Cursor ? NextCursor : Cursor + 1;
		Characters++;
	}
	return Characters;
}

#endif
