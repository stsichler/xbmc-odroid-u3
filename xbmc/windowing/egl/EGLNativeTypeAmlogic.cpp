/*
 *      Copyright (C) 2011-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "EGLNativeTypeAmlogic.h"
#include "guilib/GraphicContext.h"
#include "guilib/gui3d.h"
#include "utils/AMLUtils.h"
#include "utils/StringUtils.h"
#include "utils/SysfsUtils.h"
#include "utils/log.h"

#include <stdlib.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <EGL/egl.h>

#ifdef CLASSNAME
#undef CLASSNAME
#endif
#define CLASSNAME "CEGLNativeTypeAmlogic"

CEGLNativeTypeAmlogic::CEGLNativeTypeAmlogic()
{
  const char *env_framebuffer = getenv("FRAMEBUFFER");

  // default to framebuffer 0
  m_framebuffer_name = "fb0";
  if (env_framebuffer)
  {
    std::string framebuffer(env_framebuffer);
    std::string::size_type start = framebuffer.find("fb");
    m_framebuffer_name = framebuffer.substr(start);
  }
  m_nativeWindow = NULL;
}

CEGLNativeTypeAmlogic::~CEGLNativeTypeAmlogic()
{
}

bool CEGLNativeTypeAmlogic::CheckCompatibility()
{
  std::string name;
  std::string modalias = "/sys/class/graphics/" + m_framebuffer_name + "/device/modalias";

  SysfsUtils::GetString(modalias, name);
  if (name.find("meson") != std::string::npos)
    return true;
  return false;
}

void CEGLNativeTypeAmlogic::Initialize()
{
  aml_permissions();
  FreeScale(false);
}

void CEGLNativeTypeAmlogic::Destroy()
{
  SetScreenScale(1, 1, false);
  return;
}

bool CEGLNativeTypeAmlogic::CreateNativeDisplay()
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

  m_nativeDisplay = EGL_DEFAULT_DISPLAY;
  return true;
}

bool CEGLNativeTypeAmlogic::CreateNativeWindow()
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

#if defined(_FBDEV_WINDOW_H_)
  fbdev_window *nativeWindow = new fbdev_window;
  if (!nativeWindow)
    return false;

  RESOLUTION_INFO res;
  GetPreferredResolution(&res);

  nativeWindow->width = res.iWidth;
  nativeWindow->height = res.iHeight;
  m_nativeWindow = nativeWindow;

  SetFramebufferResolution(nativeWindow->width, nativeWindow->height);

  return true;
#else
  return false;
#endif
}

bool CEGLNativeTypeAmlogic::GetNativeDisplay(XBNativeDisplayType **nativeDisplay) const
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

  if (!nativeDisplay)
    return false;
  *nativeDisplay = (XBNativeDisplayType*) &m_nativeDisplay;
  return true;
}

bool CEGLNativeTypeAmlogic::GetNativeWindow(XBNativeWindowType **nativeWindow) const
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

  if (!nativeWindow)
    return false;
  *nativeWindow = (XBNativeWindowType*) &m_nativeWindow;
  return true;
}

bool CEGLNativeTypeAmlogic::DestroyNativeDisplay()
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

  return true;
}

bool CEGLNativeTypeAmlogic::DestroyNativeWindow()
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

#if defined(_FBDEV_WINDOW_H_)
  delete (fbdev_window*)m_nativeWindow, m_nativeWindow = NULL;
#endif
  return true;
}

bool CEGLNativeTypeAmlogic::GetNativeResolution(RESOLUTION_INFO *res) const
{
  std::string mode;
  SysfsUtils::GetString("/sys/class/display/mode", mode);
  return aml_mode_to_resolution(mode.c_str(), res);
}

bool CEGLNativeTypeAmlogic::SetNativeResolution(const RESOLUTION_INFO &res)
{
  CLog::Log(LOGNOTICE, "%s::%s to %dx%d@%f", CLASSNAME, __func__, res.iScreenWidth, res.iScreenHeight, res.fRefreshRate);
  bool result = false;

#if defined(_FBDEV_WINDOW_H_)
  if (m_nativeWindow)
  {
    ((fbdev_window *)m_nativeWindow)->width = res.iScreenWidth;
    ((fbdev_window *)m_nativeWindow)->height = res.iScreenHeight;
  }
#endif

  // Don't set the same mode as current
  std::string mode;
  SysfsUtils::GetString("/sys/class/display/mode", mode);
  
  if (res.strId != mode)
    result = SetDisplayResolution(res.strId.c_str());

  RENDER_STEREO_MODE stereo_mode = g_graphicsContext.GetStereoMode();
  aml_handle_display_stereo_mode(stereo_mode);
  
  return result;
}

bool CEGLNativeTypeAmlogic::ProbeResolutions(std::vector<RESOLUTION_INFO> &resolutions)
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

  std::string valstr;
  SysfsUtils::GetString("/sys/class/amhdmitx/amhdmitx0/disp_cap", valstr);
  std::vector<std::string> probe_str = StringUtils::Split(valstr, "\n");

  resolutions.clear();
  RESOLUTION_INFO res;
  for (std::vector<std::string>::const_iterator i = probe_str.begin(); i != probe_str.end(); ++i)
  {
    if(aml_mode_to_resolution(i->c_str(), &res))
      resolutions.push_back(res);
  }
  return resolutions.size() > 0;
}

bool CEGLNativeTypeAmlogic::GetPreferredResolution(RESOLUTION_INFO *res) const
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

  // check display/mode, it gets defaulted at boot
  if (!GetNativeResolution(res))
  {
    // punt to 720p if we get nothing
    aml_mode_to_resolution("720p", res);
  }

  return true;
}

bool CEGLNativeTypeAmlogic::ShowWindow(bool show)
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

  std::string blank_framebuffer = "/sys/class/graphics/" + m_framebuffer_name + "/blank";
  SysfsUtils::SetInt(blank_framebuffer.c_str(), show ? 0 : 1);
  return true;
}

bool CEGLNativeTypeAmlogic::SetDisplayResolution(const char *resolution)
{
  CLog::Log(LOGNOTICE, "%s::%s to %s", CLASSNAME, __func__, resolution);

  std::string mode = resolution;
  // switch display resolution
  SysfsUtils::SetString("/sys/class/display/mode", mode.c_str());

  RESOLUTION_INFO res;
  aml_mode_to_resolution(mode.c_str(), &res);
  SetFramebufferResolution(res);
  DealWithScale(res);

  return true;
}

void CEGLNativeTypeAmlogic::FreeScale(bool state)
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

  std::string freescale_framebuffer = "/sys/class/graphics/" + m_framebuffer_name + "/free_scale";
  SysfsUtils::SetInt(freescale_framebuffer.c_str(), state ? 1 : 0);
}

void CEGLNativeTypeAmlogic::DealWithScale(const RESOLUTION_INFO &res)
{
  CLog::Log(LOGDEBUG, "%s::%s Interface is %dx%d, screen size is %dx%d", CLASSNAME, __func__, res.iWidth, res.iHeight, res.iScreenWidth, res.iScreenHeight);

  if (res.iScreenWidth > res.iWidth && res.iScreenHeight > res.iHeight) {
    CLog::Log(LOGNOTICE, "%s::%s Scaling interfaces of size %dx%d to full screen", CLASSNAME, __func__, res.iWidth, res.iHeight);
    SetScreenScale(res.iWidth, res.iHeight, true);
  }
}

void CEGLNativeTypeAmlogic::SetScreenScale(int width, int height, bool state)
{
  char setting[256] = {};
  sprintf(setting, "0 0 %d %d", width - 1 , height - 1);
  std::string framebuffer = "/sys/class/graphics/" + m_framebuffer_name;
  SysfsUtils::SetString(framebuffer + "/scale_axis", setting);
  SysfsUtils::SetString(framebuffer + "/scale", state ? "0x10001" : "0x0");
}

bool CEGLNativeTypeAmlogic::IsHdmiConnected() const
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

  std::string hpd_state;
  SysfsUtils::GetString("/sys/class/amhdmitx/amhdmitx0/disp_cap", hpd_state);
  StringUtils::Trim(hpd_state);
  return hpd_state == "1";
}

void CEGLNativeTypeAmlogic::SetFramebufferResolution(const RESOLUTION_INFO &res) const
{
  SetFramebufferResolution(res.iScreenWidth, res.iScreenHeight);
}

void CEGLNativeTypeAmlogic::SetFramebufferResolution(int width, int height) const
{
  int fd0;
  std::string framebuffer = "/dev/" + m_framebuffer_name;

  if ((fd0 = open(framebuffer.c_str(), O_RDWR)) >= 0)
  {
    struct fb_var_screeninfo vinfo;
    if (ioctl(fd0, FBIOGET_VSCREENINFO, &vinfo) == 0)
    {
      vinfo.xres = width;
      vinfo.yres = height;
      vinfo.xres_virtual = width;
      vinfo.yres_virtual = height * 2;
      vinfo.bits_per_pixel = 32;
      vinfo.activate = FB_ACTIVATE_ALL;
      ioctl(fd0, FBIOPUT_VSCREENINFO, &vinfo);
    }
    close(fd0);
  }
}
