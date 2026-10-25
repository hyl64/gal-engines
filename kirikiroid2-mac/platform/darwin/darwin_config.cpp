/*
 * This file is part of the macOS port of Kirikiroid2 (zeas2/Kirikiroid2).
 *
 * darwin_config.cpp - the per-game preference file, without cocos2d
 *
 * The engine reads a few settings from a preference file that the application
 * layer owns (environ/ConfigManager/*): hinting, the font alias table, the
 * window options.  Upstream that layer is built on cocos2d's file utils; the
 * desktop host has no cocos, so this is the same class implemented against the
 * file system directly.  A missing file simply yields the defaults, which is
 * what every GetValue call falls back to here.
 */

#include "tjsCommHead.h"
#include "IndividualConfigManager.h"

#include <cstdlib>
#include <string>

std::string IndividualConfigManager::GetFilePath()
{
	if(!CurrentPath.empty())
		return CurrentPath;
	const char* home = getenv("HOME");
	if(home == nullptr)
		return std::string();
	return std::string(home) + "/Library/Application Support/Kirikiri2/preference.json";
}

IndividualConfigManager* IndividualConfigManager::GetInstance()
{
	static IndividualConfigManager instance;
	return &instance;
}

void IndividualConfigManager::Clear()
{
	AllConfig.clear();
}

// the template specialisations the engine instantiates; without a preference
// file every one of them answers with the caller's default
template<>
bool IndividualConfigManager::GetValue<bool>(const std::string& name, const bool& defVal)
{
	auto it = AllConfig.find(name);
	if(it == AllConfig.end())
		return defVal;
	return it->second == "1" || it->second == "true";
}

template<>
int IndividualConfigManager::GetValue<int>(const std::string& name, const int& defVal)
{
	auto it = AllConfig.find(name);
	return it == AllConfig.end() ? defVal : atoi(it->second.c_str());
}

template<>
float IndividualConfigManager::GetValue<float>(const std::string& name, const float& defVal)
{
	auto it = AllConfig.find(name);
	return it == AllConfig.end() ? defVal : (float)atof(it->second.c_str());
}

template<>
std::string IndividualConfigManager::GetValue<std::string>(const std::string& name, const std::string& defVal)
{
	auto it = AllConfig.find(name);
	return it == AllConfig.end() ? defVal : it->second;
}

// ---------------------------------------------------------------------------
// the locale strings
// ---------------------------------------------------------------------------
//
// environ/ConfigManager/LocaleConfigManager.cpp reads locale XML through
// cocos2d's file utils.  The desktop host has no locale pack, so the same class
// answers every id with the id itself: the engine's messages are already English
// in the catalogue it carries, and a missing translation reads as the key.

#include "LocaleConfigManager.h"

LocaleConfigManager* LocaleConfigManager::GetInstance()
{
	static LocaleConfigManager instance;
	return &instance;
}

const std::string& LocaleConfigManager::GetText(const std::string& tid)
{
	static std::string last;
	last = tid;
	return last;
}

LocaleConfigManager::LocaleConfigManager()
{
}
