// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/metro_viewer/chrome_metro_viewer_process_host_aurawin.h"

#include "ash/display/display_info.h"
#include "ash/display/display_manager.h"
#include "ash/shell.h"
#include "ash/wm/window_positioner.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/strings/stringprintf.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/browser_process_platform_part_aurawin.h"
#include "chrome/browser/browser_shutdown.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/search_engines/util.h"
#include "chrome/browser/ui/ash/ash_init.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/host_desktop.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/env_vars.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/gpu_data_manager.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/web_contents.h"
#include "ui/aura/remote_root_window_host_win.h"
#include "ui/surface/accelerated_surface_win.h"
#include "url/gurl.h"

namespace {

void CloseOpenAshBrowsers() {
  BrowserList* browser_list =
      BrowserList::GetInstance(chrome::HOST_DESKTOP_TYPE_ASH);
  if (browser_list) {
    for (BrowserList::const_iterator i = browser_list->begin();
         i != browser_list->end(); ++i) {
      Browser* browser = *i;
      browser->window()->Close();
      // If the attempt to Close the browser fails due to unload handlers on
      // the page or in progress downloads, etc, destroy all tabs on the page.
      while (browser->tab_strip_model()->count())
        delete browser->tab_strip_model()->GetWebContentsAt(0);
    }
  }
}

void OpenURL(const GURL& url) {
  chrome::NavigateParams params(
      ProfileManager::GetActiveUserProfileOrOffTheRecord(),
      GURL(url),
      content::PAGE_TRANSITION_TYPED);
  params.disposition = NEW_FOREGROUND_TAB;
  params.host_desktop_type = chrome::HOST_DESKTOP_TYPE_ASH;
  chrome::Navigate(&params);
}

}  // namespace

ChromeMetroViewerProcessHost::ChromeMetroViewerProcessHost()
    : MetroViewerProcessHost(
          content::BrowserThread::GetMessageLoopProxyForThread(
              content::BrowserThread::IO)) {
  g_browser_process->AddRefModule();
}

void ChromeMetroViewerProcessHost::OnChannelError() {
  // TODO(cpu): At some point we only close the browser. Right now this
  // is very convenient for developing.
  DVLOG(1) << "viewer channel error : Quitting browser";

  // Unset environment variable to let breakpad know that metro process wasn't
  // connected.
  ::SetEnvironmentVariableA(env_vars::kMetroConnected, NULL);

  aura::RemoteRootWindowHostWin::Instance()->Disconnected();
  g_browser_process->ReleaseModule();

  // If browser is trying to quit, we shouldn't reenter the process.
  // TODO(shrikant): In general there seem to be issues with how AttemptExit
  // reentry works. In future release please clean up related code.
  if (!browser_shutdown::IsTryingToQuit()) {
    CloseOpenAshBrowsers();
    chrome::CloseAsh();
  }
  // Tell the rest of Chrome about it.
  content::NotificationService::current()->Notify(
      chrome::NOTIFICATION_ASH_SESSION_ENDED,
      content::NotificationService::AllSources(),
      content::NotificationService::NoDetails());

  // This will delete the MetroViewerProcessHost object. Don't access member
  // variables/functions after this call.
  g_browser_process->platform_part()->OnMetroViewerProcessTerminated();
}

void ChromeMetroViewerProcessHost::OnChannelConnected(int32 /*peer_pid*/) {
  DVLOG(1) << "ChromeMetroViewerProcessHost::OnChannelConnected: ";
  // Set environment variable to let breakpad know that metro process was
  // connected.
  ::SetEnvironmentVariableA(env_vars::kMetroConnected, "1");

  if (!content::GpuDataManager::GetInstance()->GpuAccessAllowed(NULL)) {
    DVLOG(1) << "No GPU access, attempting to restart in Desktop\n";
    chrome::AttemptRestartToDesktopMode();
  }
}

void ChromeMetroViewerProcessHost::OnSetTargetSurface(
    gfx::NativeViewId target_surface) {
  HWND hwnd = reinterpret_cast<HWND>(target_surface);
  // Tell our root window host that the viewer has connected.
  aura::RemoteRootWindowHostWin::Instance()->Connected(this, hwnd);
  // Now start the Ash shell environment.
  chrome::OpenAsh();
  ash::Shell::GetInstance()->CreateLauncher();
  ash::Shell::GetInstance()->ShowLauncher();
  // On Windows 8 ASH we default to SHOW_STATE_MAXIMIZED for the browser
  // window. This is to ensure that we honor metro app conventions by default.
  ash::WindowPositioner::SetMaximizeFirstWindow(true);
  // Tell the rest of Chrome that Ash is running.
  content::NotificationService::current()->Notify(
      chrome::NOTIFICATION_ASH_SESSION_STARTED,
      content::NotificationService::AllSources(),
      content::NotificationService::NoDetails());
}

void ChromeMetroViewerProcessHost::OnOpenURL(const base::string16& url) {
  OpenURL(GURL(url));
}

void ChromeMetroViewerProcessHost::OnHandleSearchRequest(
    const base::string16& search_string) {
  GURL url(GetDefaultSearchURLForSearchTerms(
      ProfileManager::GetActiveUserProfileOrOffTheRecord(), search_string));
  if (url.is_valid())
    OpenURL(url);
}

void ChromeMetroViewerProcessHost::OnWindowSizeChanged(uint32 width,
                                                       uint32 height) {
  std::vector<ash::internal::DisplayInfo> info_list;
  info_list.push_back(ash::internal::DisplayInfo::CreateFromSpec(
      base::StringPrintf("%dx%d", width, height)));
  ash::Shell::GetInstance()->display_manager()->OnNativeDisplaysChanged(
      info_list);
  aura::RemoteRootWindowHostWin::Instance()->HandleWindowSizeChanged(width,
                                                                     height);
}
