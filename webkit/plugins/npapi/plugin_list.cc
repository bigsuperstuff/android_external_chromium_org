// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "webkit/plugins/npapi/plugin_list.h"

#include <algorithm>

#include "base/command_line.h"
#include "base/lazy_instance.h"
#include "base/logging.h"
#include "base/string_split.h"
#include "base/string_util.h"
#include "base/sys_string_conversions.h"
#include "base/utf_string_conversions.h"
#include "googleurl/src/gurl.h"
#include "net/base/mime_util.h"
#include "webkit/glue/webkit_glue.h"
#include "webkit/plugins/npapi/plugin_lib.h"
#include "webkit/plugins/plugin_switches.h"

#if defined(OS_WIN)
#include "webkit/plugins/npapi/plugin_constants_win.h"
#endif

namespace {

using webkit::npapi::PluginList;
typedef PluginList::CustomLazyInstanceTraits CustomLazyInstanceTraits;

const char kApplicationOctetStream[] = "application/octet-stream";

base::LazyInstance<PluginList, CustomLazyInstanceTraits> g_singleton =
    LAZY_INSTANCE_INITIALIZER;

bool AllowMimeTypeMismatch(const std::string& orig_mime_type,
                           const std::string& actual_mime_type) {
  if (orig_mime_type == actual_mime_type) {
    NOTREACHED();
    return true;
  }

  // We do not permit URL-sniff based plug-in MIME type overrides aside from
  // the case where the "type" was initially missing or generic
  // (application/octet-stream).
  // We collected stats to determine this approach isn't a major compat issue,
  // and we defend against content confusion attacks in various cases, such
  // as when the user doesn't have the Flash plug-in enabled.
  bool allow = orig_mime_type.empty() ||
               orig_mime_type == kApplicationOctetStream;
  LOG_IF(INFO, !allow) << "Ignoring plugin with unexpected MIME type "
                       << actual_mime_type << " (expected " << orig_mime_type
                       << ")";
  return allow;
}

}  // namespace

namespace webkit {
namespace npapi {

struct PluginList::CustomLazyInstanceTraits
    : base::DefaultLazyInstanceTraits<PluginList> {
  static PluginList* New(void* instance) {
    PluginList* plugin_list =
        base::DefaultLazyInstanceTraits<PluginList>::New(instance);
    plugin_list->PlatformInit();
    return plugin_list;
  }
};

// static
PluginList* PluginList::Singleton() {
  return g_singleton.Pointer();
}

// static
bool PluginList::DebugPluginLoading() {
  return CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kDebugPluginLoading);
}

void PluginList::RefreshPlugins() {
  base::AutoLock lock(lock_);
  loading_state_ = LOADING_STATE_NEEDS_REFRESH;
}

void PluginList::AddExtraPluginPath(const FilePath& plugin_path) {
  // Chrome OS only loads plugins from /opt/google/chrome/plugins.
#if !defined(OS_CHROMEOS)
  base::AutoLock lock(lock_);
  extra_plugin_paths_.push_back(plugin_path);
#endif
}

void PluginList::RemoveExtraPluginPath(const FilePath& plugin_path) {
  base::AutoLock lock(lock_);
  std::vector<FilePath>::iterator it =
      std::find(extra_plugin_paths_.begin(), extra_plugin_paths_.end(),
                plugin_path);
  if (it != extra_plugin_paths_.end())
    extra_plugin_paths_.erase(it);
}

void PluginList::AddExtraPluginDir(const FilePath& plugin_dir) {
  // Chrome OS only loads plugins from /opt/google/chrome/plugins.
#if !defined(OS_CHROMEOS)
  base::AutoLock lock(lock_);
  extra_plugin_dirs_.push_back(plugin_dir);
#endif
}

void PluginList::RegisterInternalPlugin(const webkit::WebPluginInfo& info,
                                        bool add_at_beginning) {
  PluginEntryPoints entry_points = {0};
  RegisterInternalPluginWithEntryPoints(info, add_at_beginning, entry_points);
}

void PluginList::RegisterInternalPluginWithEntryPoints(
    const webkit::WebPluginInfo& info,
    bool add_at_beginning,
    const PluginEntryPoints& entry_points) {
  InternalPlugin plugin = { info, entry_points };

  base::AutoLock lock(lock_);

  internal_plugins_.push_back(plugin);
  if (add_at_beginning) {
    // Newer registrations go earlier in the list so they can override the MIME
    // types of older registrations.
    extra_plugin_paths_.insert(extra_plugin_paths_.begin(), info.path);
  } else {
    extra_plugin_paths_.push_back(info.path);
  }
}

void PluginList::UnregisterInternalPlugin(const FilePath& path) {
  base::AutoLock lock(lock_);
  for (size_t i = 0; i < internal_plugins_.size(); i++) {
    if (internal_plugins_[i].info.path == path) {
      internal_plugins_.erase(internal_plugins_.begin() + i);
      return;
    }
  }
  NOTREACHED();
}

void PluginList::GetInternalPlugins(
    std::vector<webkit::WebPluginInfo>* internal_plugins) {
  base::AutoLock lock(lock_);

  for (std::vector<InternalPlugin>::iterator it = internal_plugins_.begin();
       it != internal_plugins_.end();
       ++it) {
    internal_plugins->push_back(it->info);
  }
}

bool PluginList::ReadPluginInfo(const FilePath& filename,
                                webkit::WebPluginInfo* info,
                                const PluginEntryPoints** entry_points) {
  {
    base::AutoLock lock(lock_);
    for (size_t i = 0; i < internal_plugins_.size(); ++i) {
      if (filename == internal_plugins_[i].info.path) {
        *entry_points = &internal_plugins_[i].entry_points;
        *info = internal_plugins_[i].info;
        return true;
      }
    }
  }

  // Not an internal plugin.
  *entry_points = NULL;

  return PluginLib::ReadWebPluginInfo(filename, info);
}

// static
bool PluginList::ParseMimeTypes(
    const std::string& mime_types_str,
    const std::string& file_extensions_str,
    const string16& mime_type_descriptions_str,
    std::vector<webkit::WebPluginMimeType>* parsed_mime_types) {
  std::vector<std::string> mime_types, file_extensions;
  std::vector<string16> descriptions;
  base::SplitString(mime_types_str, '|', &mime_types);
  base::SplitString(file_extensions_str, '|', &file_extensions);
  base::SplitString(mime_type_descriptions_str, '|', &descriptions);

  parsed_mime_types->clear();

  if (mime_types.empty())
    return false;

  for (size_t i = 0; i < mime_types.size(); ++i) {
    WebPluginMimeType mime_type;
    mime_type.mime_type = StringToLowerASCII(mime_types[i]);
    if (file_extensions.size() > i)
      base::SplitString(file_extensions[i], ',', &mime_type.file_extensions);

    if (descriptions.size() > i) {
      mime_type.description = descriptions[i];

      // On Windows, the description likely has a list of file extensions
      // embedded in it (e.g. "SurfWriter file (*.swr)"). Remove an extension
      // list from the description if it is present.
      size_t ext = mime_type.description.find(ASCIIToUTF16("(*"));
      if (ext != string16::npos) {
        if (ext > 1 && mime_type.description[ext - 1] == ' ')
          ext--;

        mime_type.description.erase(ext);
      }
    }

    parsed_mime_types->push_back(mime_type);
  }

  return true;
}

PluginList::PluginList()
    :
#if defined(OS_WIN)
      dont_load_new_wmp_(false),
#endif
      loading_state_(LOADING_STATE_NEEDS_REFRESH) {
}

void PluginList::LoadPluginsIntoPluginListInternal(
    std::vector<webkit::WebPluginInfo>* plugins) {
  base::Closure will_load_callback;
  {
    base::AutoLock lock(lock_);
    will_load_callback = will_load_plugins_callback_;
  }
  if (!will_load_callback.is_null())
    will_load_callback.Run();

  std::vector<FilePath> plugin_paths;
  GetPluginPathsToLoad(&plugin_paths);

  for (std::vector<FilePath>::const_iterator it = plugin_paths.begin();
       it != plugin_paths.end();
       ++it) {
    WebPluginInfo plugin_info;
    LoadPluginIntoPluginList(*it, plugins, &plugin_info);
  }
}

void PluginList::LoadPlugins() {
  {
    base::AutoLock lock(lock_);
    if (loading_state_ == LOADING_STATE_UP_TO_DATE)
      return;

    loading_state_ = LOADING_STATE_REFRESHING;
  }

  std::vector<webkit::WebPluginInfo> new_plugins;
  // Do the actual loading of the plugins.
  LoadPluginsIntoPluginListInternal(&new_plugins);

  base::AutoLock lock(lock_);
  plugins_list_.swap(new_plugins);

  // If we haven't been invalidated in the mean time, mark the plug-in list as
  // up-to-date.
  if (loading_state_ != LOADING_STATE_NEEDS_REFRESH)
    loading_state_ = LOADING_STATE_UP_TO_DATE;
}

bool PluginList::LoadPluginIntoPluginList(
    const FilePath& path,
    std::vector<webkit::WebPluginInfo>* plugins,
    WebPluginInfo* plugin_info) {
  LOG_IF(ERROR, PluginList::DebugPluginLoading())
      << "Loading plugin " << path.value();
  const PluginEntryPoints* entry_points;

  if (!ReadPluginInfo(path, plugin_info, &entry_points))
    return false;

  if (!ShouldLoadPluginUsingPluginList(*plugin_info, plugins))
    return false;

#if defined(OS_WIN) && !defined(NDEBUG)
  if (path.BaseName().value() != L"npspy.dll")  // Make an exception for NPSPY
#endif
  {
    for (size_t i = 0; i < plugin_info->mime_types.size(); ++i) {
      // TODO: don't load global handlers for now.
      // WebKit hands to the Plugin before it tries
      // to handle mimeTypes on its own.
      const std::string &mime_type = plugin_info->mime_types[i].mime_type;
      if (mime_type == "*")
        return false;
    }
  }
  plugins->push_back(*plugin_info);
  return true;
}

void PluginList::GetPluginPathsToLoad(std::vector<FilePath>* plugin_paths) {
  // Don't want to hold the lock while loading new plugins, so we don't block
  // other methods if they're called on other threads.
  std::vector<FilePath> extra_plugin_paths;
  std::vector<FilePath> extra_plugin_dirs;
  {
    base::AutoLock lock(lock_);
    extra_plugin_paths = extra_plugin_paths_;
    extra_plugin_dirs = extra_plugin_dirs_;
  }

  std::vector<FilePath> directories_to_scan;
  GetPluginDirectories(&directories_to_scan);

  for (size_t i = 0; i < extra_plugin_paths.size(); ++i) {
    const FilePath& path = extra_plugin_paths[i];
    if (std::find(plugin_paths->begin(), plugin_paths->end(), path) !=
        plugin_paths->end()) {
      continue;
    }
    plugin_paths->push_back(path);
  }

  for (size_t i = 0; i < extra_plugin_dirs.size(); ++i)
    GetPluginsInDir(extra_plugin_dirs[i], plugin_paths);

  for (size_t i = 0; i < directories_to_scan.size(); ++i)
    GetPluginsInDir(directories_to_scan[i], plugin_paths);

#if defined(OS_WIN)
  GetPluginPathsFromRegistry(plugin_paths);
#endif
}

void PluginList::SetPlugins(const std::vector<webkit::WebPluginInfo>& plugins) {
  base::AutoLock lock(lock_);

  DCHECK_NE(LOADING_STATE_REFRESHING, loading_state_);
  loading_state_ = LOADING_STATE_UP_TO_DATE;

  plugins_list_.clear();
  plugins_list_.insert(plugins_list_.end(), plugins.begin(), plugins.end());
}

void PluginList::set_will_load_plugins_callback(const base::Closure& callback) {
  base::AutoLock lock(lock_);
  will_load_plugins_callback_ = callback;
}

void PluginList::GetPlugins(std::vector<WebPluginInfo>* plugins) {
  LoadPlugins();
  base::AutoLock lock(lock_);
  plugins->insert(plugins->end(), plugins_list_.begin(), plugins_list_.end());
}

bool PluginList::GetPluginsNoRefresh(
    std::vector<webkit::WebPluginInfo>* plugins) {
  base::AutoLock lock(lock_);
  plugins->insert(plugins->end(), plugins_list_.begin(), plugins_list_.end());

  return loading_state_ == LOADING_STATE_UP_TO_DATE;
}

void PluginList::GetPluginInfoArray(
    const GURL& url,
    const std::string& mime_type,
    bool allow_wildcard,
    bool* use_stale,
    std::vector<webkit::WebPluginInfo>* info,
    std::vector<std::string>* actual_mime_types) {
  DCHECK(mime_type == StringToLowerASCII(mime_type));
  DCHECK(info);

  if (!use_stale)
    LoadPlugins();
  base::AutoLock lock(lock_);
  if (use_stale)
    *use_stale = (loading_state_ != LOADING_STATE_UP_TO_DATE);
  info->clear();
  if (actual_mime_types)
    actual_mime_types->clear();

  std::set<FilePath> visited_plugins;

  // Add in plugins by mime type.
  for (size_t i = 0; i < plugins_list_.size(); ++i) {
    if (SupportsType(plugins_list_[i], mime_type, allow_wildcard)) {
      FilePath path = plugins_list_[i].path;
      if (visited_plugins.insert(path).second) {
        info->push_back(plugins_list_[i]);
        if (actual_mime_types)
          actual_mime_types->push_back(mime_type);
      }
    }
  }

  // Add in plugins by url.
  std::string path = url.path();
  std::string::size_type last_dot = path.rfind('.');
  if (last_dot != std::string::npos) {
    std::string extension = StringToLowerASCII(std::string(path, last_dot+1));
    std::string actual_mime_type;
    for (size_t i = 0; i < plugins_list_.size(); ++i) {
      if (SupportsExtension(plugins_list_[i], extension, &actual_mime_type)) {
        FilePath path = plugins_list_[i].path;
        if (visited_plugins.insert(path).second &&
            AllowMimeTypeMismatch(mime_type, actual_mime_type)) {
          info->push_back(plugins_list_[i]);
          if (actual_mime_types)
            actual_mime_types->push_back(actual_mime_type);
        }
      }
    }
  }
}

bool PluginList::SupportsType(const webkit::WebPluginInfo& plugin,
                              const std::string& mime_type,
                              bool allow_wildcard) {
  // Webkit will ask for a plugin to handle empty mime types.
  if (mime_type.empty())
    return false;

  for (size_t i = 0; i < plugin.mime_types.size(); ++i) {
    const webkit::WebPluginMimeType& mime_info = plugin.mime_types[i];
    if (net::MatchesMimeType(mime_info.mime_type, mime_type)) {
      if (!allow_wildcard && mime_info.mime_type == "*")
        continue;
      return true;
    }
  }
  return false;
}

bool PluginList::SupportsExtension(const webkit::WebPluginInfo& plugin,
                                   const std::string& extension,
                                   std::string* actual_mime_type) {
  for (size_t i = 0; i < plugin.mime_types.size(); ++i) {
    const webkit::WebPluginMimeType& mime_type = plugin.mime_types[i];
    for (size_t j = 0; j < mime_type.file_extensions.size(); ++j) {
      if (mime_type.file_extensions[j] == extension) {
        if (actual_mime_type)
          *actual_mime_type = mime_type.mime_type;
        return true;
      }
    }
  }
  return false;
}

/*static*/
bool PluginList::RemovePlugin(const FilePath& filename,
                              std::vector<webkit::WebPluginInfo>* plugins) {
  bool did_remove = false;
  for (size_t i = 0; i < plugins->size();) {
    if ((*plugins)[i].path == filename) {
      plugins->erase(plugins->begin() + i);
      did_remove = true;
    } else {
      i++;
    }
  }
  return did_remove;
}

PluginList::~PluginList() {
}


}  // namespace npapi
}  // namespace webkit