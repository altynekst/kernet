/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "music_smtc.h"

#include <base/system.h>
#include <base/thread.h>

#include <chrono>
#include <thread>

#if defined(CONF_FAMILY_WINDOWS)

#include <base/windows.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Control;

namespace
{
	enum class ECommand : int
	{
		NONE = 0,
		TOGGLE_PLAY_PAUSE,
		NEXT,
		PREV,
	};

	static void SetIfDifferent(std::string &Dst, const std::string &Src)
	{
		if(Dst != Src)
			Dst = Src;
	}

	static std::string Utf8FromHString(const winrt::hstring &Str)
	{
		auto Opt = windows_wide_to_utf8(Str.c_str());
		return Opt.value_or("");
	}
}

class CMusicSMTC::CWorker
{
public:
	CMusicSMTC *m_pSelf;
	std::atomic_int m_PendingCmd{(int)ECommand::NONE};

	CWorker(CMusicSMTC *pSelf) : m_pSelf(pSelf) {}

	void Run()
	{
		// thread_init initializes COM, but we also need WinRT apartment.
		winrt::init_apartment(winrt::apartment_type::multi_threaded);

		GlobalSystemMediaTransportControlsSessionManager Manager{nullptr};
		GlobalSystemMediaTransportControlsSession Session{nullptr};

		while(!m_pSelf->m_StopRequested.load())
		{
			// Ensure manager
			if(!Manager)
			{
				try
				{
					Manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
				}
				catch(...)
				{
					UpdateUnavailable();
					std::this_thread::sleep_for(std::chrono::milliseconds(1000));
					continue;
				}
			}

			// Refresh current session (might change when user changes active player)
			try
			{
				Session = Manager.GetCurrentSession();
			}
			catch(...)
			{
				Session = nullptr;
			}

			// Execute pending command on the current session
			const ECommand Cmd = (ECommand)m_PendingCmd.exchange((int)ECommand::NONE);
			if(Cmd != ECommand::NONE)
			{
				TryExecute(Session, Cmd);
			}

			// Poll info
			Poll(Session);

			std::this_thread::sleep_for(std::chrono::milliseconds(300));
		}
		winrt::uninit_apartment();
	}

private:
	void UpdateUnavailable()
	{
		std::scoped_lock Lock(m_pSelf->m_Mutex);
		m_pSelf->m_TrackInfo = {};
		m_pSelf->m_TrackInfo.m_Available = false;
		m_pSelf->m_TrackInfo.m_Playing = false;
	}

	void TryExecute(const GlobalSystemMediaTransportControlsSession &Session, ECommand Cmd)
	{
		if(!Session)
			return;
		try
		{
			switch(Cmd)
			{
			case ECommand::TOGGLE_PLAY_PAUSE:
				Session.TryTogglePlayPauseAsync().get();
				break;
			case ECommand::NEXT:
				Session.TrySkipNextAsync().get();
				break;
			case ECommand::PREV:
				Session.TrySkipPreviousAsync().get();
				break;
			default:
				break;
			}
		}
		catch(...)
		{
			// ignore
		}
	}

	void Poll(const GlobalSystemMediaTransportControlsSession &Session)
	{
		if(!Session)
		{
			UpdateUnavailable();
			return;
		}

		bool Playing = false;
		try
		{
			auto PlaybackInfo = Session.GetPlaybackInfo();
			if(PlaybackInfo)
			{
				const auto Status = PlaybackInfo.PlaybackStatus();
				Playing = Status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
			}
		}
		catch(...)
		{
			// keep default
		}

		std::string Title;
		std::string Artist;
		try
		{
			auto Props = Session.TryGetMediaPropertiesAsync().get();
			Title = Utf8FromHString(Props.Title());
			Artist = Utf8FromHString(Props.Artist());
		}
		catch(...)
		{
			// ignore
		}

		{
			std::scoped_lock Lock(m_pSelf->m_Mutex);
			m_pSelf->m_TrackInfo.m_Available = true;
			m_pSelf->m_TrackInfo.m_Playing = Playing;
			SetIfDifferent(m_pSelf->m_TrackInfo.m_Title, Title);
			SetIfDifferent(m_pSelf->m_TrackInfo.m_Artist, Artist);
		}
	}
};

CMusicSMTC::CMusicSMTC() = default;
CMusicSMTC::~CMusicSMTC() = default;

void CMusicSMTC::OnInit()
{
	StartWorker();
}

void CMusicSMTC::OnShutdown()
{
	StopWorker();
}

void CMusicSMTC::OnUpdate()
{
	// no-op, worker updates state
}

CMusicSMTC::STrackInfo CMusicSMTC::GetTrackInfo() const
{
	std::scoped_lock Lock(m_Mutex);
	return m_TrackInfo;
}

void CMusicSMTC::TogglePlayPause()
{
	if(!m_pWorker)
		return;
	((CWorker *)m_pWorker)->m_PendingCmd.store((int)ECommand::TOGGLE_PLAY_PAUSE);
}

void CMusicSMTC::Next()
{
	if(!m_pWorker)
		return;
	((CWorker *)m_pWorker)->m_PendingCmd.store((int)ECommand::NEXT);
}

void CMusicSMTC::Previous()
{
	if(!m_pWorker)
		return;
	((CWorker *)m_pWorker)->m_PendingCmd.store((int)ECommand::PREV);
}

void CMusicSMTC::StartWorker()
{
	if(m_WorkerRunning.exchange(true))
		return;
	m_StopRequested.store(false);
	m_pWorker = new CWorker(this);
	m_pThread = thread_init(CMusicSMTC::ThreadMain, m_pWorker, "smtc");
}

void CMusicSMTC::StopWorker()
{
	m_StopRequested.store(true);
	if(m_pThread)
	{
		thread_wait(m_pThread);
		m_pThread = nullptr;
	}
	if(m_pWorker)
	{
		delete (CWorker *)m_pWorker;
		m_pWorker = nullptr;
	}
	m_WorkerRunning.store(false);
}

void CMusicSMTC::ThreadMain(void *pUser)
{
	CWorker *pWorker = (CWorker *)pUser;
	pWorker->Run();
}

#else

CMusicSMTC::CMusicSMTC() = default;
CMusicSMTC::~CMusicSMTC() = default;

void CMusicSMTC::OnInit() {}
void CMusicSMTC::OnShutdown() {}
void CMusicSMTC::OnUpdate() {}

CMusicSMTC::STrackInfo CMusicSMTC::GetTrackInfo() const { return {}; }
void CMusicSMTC::TogglePlayPause() {}
void CMusicSMTC::Next() {}
void CMusicSMTC::Previous() {}

void CMusicSMTC::StartWorker() {}
void CMusicSMTC::StopWorker() {}
void CMusicSMTC::ThreadMain(void *) {}

#endif
