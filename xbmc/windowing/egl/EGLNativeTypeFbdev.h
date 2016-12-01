#pragma once

/*
 *      Copyright (C) 2011-2012 Team XBMC
 *      http://www.xbmc.org
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

#include "EGLNativeType.h"
#include <linux/fb.h>
#include <list>

#ifndef _FBDEV_WINDOW_H_
// Define it right here, since some platforms doesn't has fbdev_window.h at all.
// This will not make it fail on these platforms badly, since it will fail softly anyway on some other init steps.
#define _FBDEV_WINDOW_H_
typedef struct fbdev_window
{
  unsigned short width;
  unsigned short height;
} fbdev_window;
#endif

class CEGLNativeTypeFbdev : public CEGLNativeType
{
public:
  CEGLNativeTypeFbdev();
  virtual ~CEGLNativeTypeFbdev();
  virtual std::string GetNativeName() const { return "FBDev"; };
  virtual bool  CheckCompatibility();
  virtual void  Initialize();
  virtual void  Destroy();
  virtual int   GetQuirks() { return EGL_QUIRK_NONE; };

  virtual bool  CreateNativeDisplay();
  virtual bool  CreateNativeWindow();
  virtual bool  GetNativeDisplay(XBNativeDisplayType **nativeDisplay) const;
  virtual bool  GetNativeWindow(XBNativeWindowType **nativeWindow) const;

  virtual bool  DestroyNativeWindow();
  virtual bool  DestroyNativeDisplay();

  virtual bool  GetNativeResolution(RESOLUTION_INFO *res) const;
  virtual bool  SetNativeResolution(const RESOLUTION_INFO &res);
  virtual bool  ProbeResolutions(std::vector<RESOLUTION_INFO> &resolutions);
  virtual bool  GetPreferredResolution(RESOLUTION_INFO *res) const;

  virtual bool  ShowWindow(bool show);

protected:
  int m_iFBHandle;
  fb_var_screeninfo *vinfo;
  fb_fix_screeninfo *finfo;
};
