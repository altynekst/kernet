#ifndef GAME_CLIENT_COMPONENTS_NAMEPLATES_H
#define GAME_CLIENT_COMPONENTS_NAMEPLATES_H

#include <base/color.h>
#include <base/vmath.h>

#include <engine/graphics.h>

#include <game/client/component.h>

struct CNetObj_PlayerInfo;

class CNamePlates : public CComponent
{
private:
	class CNamePlatesData;
	CNamePlatesData *m_pData;
	IGraphics::CTextureHandle m_KernelNetBadgeTexture;
	IGraphics::CTextureHandle m_KernelNetTesterBadgeTexture;
	IGraphics::CTextureHandle m_KernelNetSupporterBadgeTexture;
	void RenderChatBubble(int ClientId, vec2 NamePlateBottom, float NamePlateHeight, float Alpha);

public:
	enum class EKernelNetBadge
	{
		DEVELOPER,
		TESTER,
		SUPPORTER,
	};
	void RenderKernelNetBadge(vec2 Position, float Size, float Alpha, EKernelNetBadge Badge = EKernelNetBadge::DEVELOPER) const;
	void RenderNamePlateGame(vec2 Position, const CNetObj_PlayerInfo *pPlayerInfo, float Alpha);
	void RenderNamePlatePreview(vec2 Position, int Dummy);
	void ResetNamePlates();
	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnMessage(int MsgType, void *pRawMsg) override;
	void OnInit() override;
	void OnShutdown() override;
	void OnWindowResize() override;
	void OnRender() override;
	CNamePlates();
	~CNamePlates() override;
};

#endif
