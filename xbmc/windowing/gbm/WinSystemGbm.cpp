/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemGbm.h"
#include "ServiceBroker.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include <string.h>

#include "OptionalsReg.h"
#include "platform/linux/OptionalsReg.h"
#include "windowing/GraphicContext.h"
#include "platform/linux/powermanagement/LinuxPowerSyscall.h"
#include "settings/DisplaySettings.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "DRMAtomic.h"
#include "DRMLegacy.h"
#include "OffScreenModeSetting.h"
#include "messaging/ApplicationMessenger.h"

using namespace KODI::WINDOWING::GBM;

CWinSystemGbm::CWinSystemGbm() :
  m_DRM(nullptr),
  m_GBM(new CGBMUtils),
  m_libinput(new CLibInputHandler)
{
  OPTIONALS::ALSARegister();
  OPTIONALS::PulseAudioRegister();

  CLinuxPowerSyscall::Register();
  m_lirc.reset(OPTIONALS::LircRegister());
  m_libinput->Start();
}

bool CWinSystemGbm::InitWindowSystem()
{
  m_DRM = std::make_shared<CDRMAtomic>();

  if (!m_DRM->InitDrm())
  {
    CLog::Log(LOGERROR, "CWinSystemGbm::%s - failed to initialize Atomic DRM", __FUNCTION__);
    m_DRM.reset();

    m_DRM = std::make_shared<CDRMLegacy>();

    if (!m_DRM->InitDrm())
    {
      CLog::Log(LOGERROR, "CWinSystemGbm::%s - failed to initialize Legacy DRM", __FUNCTION__);
      m_DRM.reset();

      m_DRM = std::make_shared<COffScreenModeSetting>();
      if (!m_DRM->InitDrm())
      {
        CLog::Log(LOGERROR, "CWinSystemGbm::%s - failed to initialize off screen DRM", __FUNCTION__);
        m_DRM.reset();
        return false;
      }
    }
  }

  if (!m_GBM->CreateDevice(m_DRM->GetFileDescriptor()))
  {
    m_GBM.reset();
    return false;
  }

  CLog::Log(LOGDEBUG, "CWinSystemGbm::%s - initialized DRM", __FUNCTION__);
  return CWinSystemBase::InitWindowSystem();
}

bool CWinSystemGbm::DestroyWindowSystem()
{
  m_GBM->DestroyDevice();

  CLog::Log(LOGDEBUG, "CWinSystemGbm::%s - deinitialized DRM", __FUNCTION__);

  m_libinput.reset();

  return true;
}

void CWinSystemGbm::UpdateResolutions()
{
  RESOLUTION_INFO current = m_DRM->GetCurrentMode();

  auto resolutions = m_DRM->GetModes();
  if (resolutions.empty())
  {
    CLog::Log(LOGWARNING, "CWinSystemGbm::%s - Failed to get resolutions", __FUNCTION__);
  }
  else
  {
    CDisplaySettings::GetInstance().ClearCustomResolutions();

    for (auto &res : resolutions)
    {
      CServiceBroker::GetWinSystem()->GetGfxContext().ResetOverscan(res);
      CDisplaySettings::GetInstance().AddResolutionInfo(res);

      if (current.iScreenWidth == res.iScreenWidth &&
          current.iScreenHeight == res.iScreenHeight &&
          current.iWidth == res.iWidth &&
          current.iHeight == res.iHeight &&
          current.fRefreshRate == res.fRefreshRate &&
          current.dwFlags == res.dwFlags)
      {
        CDisplaySettings::GetInstance().GetResolutionInfo(RES_DESKTOP) = res;
      }

      CLog::Log(LOGNOTICE, "Found resolution %dx%d with %dx%d%s @ %f Hz",
                res.iWidth,
                res.iHeight,
                res.iScreenWidth,
                res.iScreenHeight,
                res.dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "",
                res.fRefreshRate);
    }
  }

  CDisplaySettings::GetInstance().ApplyCalibrations();
}

bool CWinSystemGbm::ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop)
{
  return true;
}

bool CWinSystemGbm::SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays)
{
  // Notify other subsystems that we will change resolution
  OnLostDevice();

  if(!m_DRM->SetMode(res))
  {
    CLog::Log(LOGERROR, "CWinSystemGbm::%s - failed to set DRM mode", __FUNCTION__);
    return false;
  }

  struct gbm_bo *bo = nullptr;

  if (!std::dynamic_pointer_cast<CDRMAtomic>(m_DRM))
  {
    bo = m_GBM->LockFrontBuffer();
  }

  auto result = m_DRM->SetVideoMode(res, bo);

  if (!std::dynamic_pointer_cast<CDRMAtomic>(m_DRM))
  {
    m_GBM->ReleaseBuffer();
  }

  int delay = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt("videoscreen.delayrefreshchange");
  if (delay > 0)
  {
    m_delayDispReset = true;
    m_dispResetTimer.Set(delay * 100);
  }

  return result;
}

void CWinSystemGbm::FlipPage(bool rendered, bool videoLayer)
{
  if (m_videoLayerBridge && !videoLayer)
  {
    // disable video plane when video layer no longer is active
    m_videoLayerBridge->Disable();
  }

  struct gbm_bo *bo = nullptr;

  if (rendered)
  {
    bo = m_GBM->LockFrontBuffer();
  }

  m_DRM->FlipPage(bo, rendered, videoLayer);

  if (rendered)
  {
    m_GBM->ReleaseBuffer();
  }

  if (m_videoLayerBridge && !videoLayer)
  {
    // delete video layer bridge when video layer no longer is active
    m_videoLayerBridge.reset();
  }
}

bool CWinSystemGbm::UseLimitedColor()
{
  return CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOSCREEN_LIMITEDRANGE);
}

bool CWinSystemGbm::Hide()
{
  bool ret = m_DRM->SetActive(false);
  FlipPage(false, false);
  return ret;
}

bool CWinSystemGbm::Show(bool raise)
{
  bool ret = m_DRM->SetActive(true);
  FlipPage(false, false);
  return ret;
}

void CWinSystemGbm::Register(IDispResource *resource)
{
  CSingleLock lock(m_resourceSection);
  m_resources.push_back(resource);
}

void CWinSystemGbm::Unregister(IDispResource *resource)
{
  CSingleLock lock(m_resourceSection);
  std::vector<IDispResource*>::iterator i = find(m_resources.begin(), m_resources.end(), resource);
  if (i != m_resources.end())
  {
    m_resources.erase(i);
  }
}

void CWinSystemGbm::OnLostDevice()
{
  CLog::Log(LOGDEBUG, "%s - notify display change event", __FUNCTION__);

  CSingleLock lock(m_resourceSection);
  for (auto resource : m_resources)
    resource->OnLostDisplay();
}
