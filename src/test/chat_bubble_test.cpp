#include <game/client/components/tclient/chat_bubble.h>

#include <gtest/gtest.h>

TEST(ChatBubble, OnlyPublicMessagesAreShownInWorld)
{
	EXPECT_TRUE(ShouldShowWorldChatBubble(0));
	EXPECT_TRUE(ShouldShowWorldChatBubble(1));
	EXPECT_FALSE(ShouldShowWorldChatBubble(TEAM_WHISPER_SEND));
	EXPECT_FALSE(ShouldShowWorldChatBubble(TEAM_WHISPER_RECV));
}

TEST(ChatBubble, RevealsWholeUtf8Characters)
{
	const char *pText = "a\xD0\xB1" "c";
	EXPECT_EQ(ChatBubbleCharacterCount(pText), 3);
	EXPECT_EQ(ChatBubbleRevealBytes(pText, 0), 0);
	EXPECT_EQ(ChatBubbleRevealBytes(pText, 1), 1);
	EXPECT_EQ(ChatBubbleRevealBytes(pText, 2), 3);
	EXPECT_EQ(ChatBubbleRevealBytes(pText, 3), 4);
	EXPECT_EQ(ChatBubbleRevealBytes(pText, 20), 4);
}
