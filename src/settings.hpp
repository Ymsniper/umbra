#pragma once
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Ymsniper
// Menu settings, saved to settings.cfg on exit and reloaded at
// launch. Game offsets are separate and live in offsets.cfg.
#include "global.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>

// Menu settings that survive a restart.

// run.sh starts the binary from build/, so a bare "settings.cfg" resolves to
// build/settings.cfg and the file kept with the project is never read. Resolve
// once, searching the working directory and then the parent, exactly as the
// offsets loader does, and use the same path for saving so the two do not
// diverge into separate files.
inline const std::string& settingsPath() {
    static std::string resolved;
    if (!resolved.empty()) return resolved;
    auto exists = [](const char* p) { std::ifstream f(p); return (bool)f; };
    if      (exists("settings.cfg"))     resolved = "settings.cfg";
    else if (exists("../settings.cfg"))  resolved = "../settings.cfg";
    // Neither exists yet: if we are running from build/, the project directory
    // above is where it belongs, not beside the binary.
    else if (exists("../CMakeLists.txt")) resolved = "../settings.cfg";
    else                                  resolved = "settings.cfg";
    return resolved;
}

inline const char* kSettingsFile = "settings.cfg";

struct SettingRef {
    enum Kind { BOOL, INT, FLOAT } kind;
    void* p;
};

inline std::map<std::string, SettingRef> settingFields() {
    return {
        {"esp_boxes",      {SettingRef::BOOL,  &g_espBoxes}},
        {"esp_snaplines",  {SettingRef::BOOL,  &g_espSnaplines}},
        {"esp_health",     {SettingRef::BOOL,  &g_espHealth}},
        {"esp_name",       {SettingRef::BOOL,  &g_espName}},
        {"esp_distance",   {SettingRef::BOOL,  &g_espDistance}},
        {"esp_teamcolor",  {SettingRef::BOOL,  &g_espTeamColor}},
        {"esp_teammates",  {SettingRef::BOOL,  &g_espTeammates}},
        {"esp_self",       {SettingRef::BOOL,  &g_espSelf}},
        {"esp_alpha",      {SettingRef::INT,   &g_espAlpha}},
        {"fov",            {SettingRef::FLOAT, &g_fov}},
        {"aim_enabled",    {SettingRef::BOOL,  &g_aimEnabled}},
        {"aim_button",     {SettingRef::INT,   &g_aimButton}},
        {"aim_bone",       {SettingRef::INT,   &g_aimBone}},
        {"esp_skeleton",   {SettingRef::BOOL,  &g_espSkeleton}},
        {"aim_smooth",     {SettingRef::FLOAT, &g_aimSmooth}},
        {"aim_fov_px",     {SettingRef::FLOAT, &g_aimFovPx}},
        {"aim_max_dist",   {SettingRef::FLOAT, &g_aimMaxDist}},
        {"aim_head_lift",  {SettingRef::FLOAT, &g_aimHeadLift}},
        {"aim_target_bias",{SettingRef::FLOAT, &g_aimTargetBias}},
        {"aim_sticky",     {SettingRef::BOOL,  &g_aimSticky}},
        {"aim_stickiness", {SettingRef::FLOAT, &g_aimStickiness}},
        {"aim_dist_fov",   {SettingRef::BOOL,  &g_aimDistFov}},
        {"aim_smooth_mode",{SettingRef::INT,   &g_aimSmoothMode}},
        {"aim_inertia",    {SettingRef::FLOAT, &g_aimInertia}},
        {"aim_head_fwd",   {SettingRef::FLOAT, &g_aimHeadFwd}},
        {"aim_head_up",    {SettingRef::FLOAT, &g_aimHeadUp}},
        {"trig_enabled",   {SettingRef::BOOL,  &g_trigEnabled}},
        {"trig_button",    {SettingRef::INT,   &g_trigButton}},
        {"trig_forgive",   {SettingRef::FLOAT, &g_trigForgiveness}},
        {"trig_activate_ms",{SettingRef::INT,  &g_trigActivateMs}},
        {"trig_delay_ms",  {SettingRef::INT,   &g_trigDelayMs}},
        {"trig_hold_ms",   {SettingRef::INT,   &g_trigHoldMs}},
        {"trig_cooldown",  {SettingRef::INT,   &g_trigCooldownMs}},
        {"trig_skeleton",  {SettingRef::BOOL,  &g_trigSkeleton}},
        {"trig_skel_part", {SettingRef::INT,   &g_trigSkelPart}},
        {"trig_size_scale",{SettingRef::FLOAT, &g_trigSizeScale}},
        {"trig_min_tol",   {SettingRef::FLOAT, &g_trigMinTol}},
        {"aim_predict",    {SettingRef::BOOL,  &g_aimPredict}},
        {"aim_lead_ms",    {SettingRef::FLOAT, &g_aimLeadMs}},
        {"aim_visible_only",  {SettingRef::BOOL, &g_aimVisibleOnly}},
        {"trig_visible_only", {SettingRef::BOOL, &g_trigVisibleOnly}},
        {"vis_style",         {SettingRef::INT,  &g_visStyle}},
        {"vis_dim_alpha",     {SettingRef::INT,  &g_visDimAlpha}},
        {"vis_tolerance",     {SettingRef::FLOAT,&g_visTolerance}},
        {"menu_w",         {SettingRef::FLOAT, &g_menuW}},
        {"menu_h",         {SettingRef::FLOAT, &g_menuH}},
    };
}

inline bool loadSettings(const char* path = nullptr) {
    if (!path) path = settingsPath().c_str();
    std::ifstream f(path);
    if (!f) return false;
    auto fields = settingFields();
    std::string line;
    int n = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && isspace((unsigned char)k.back()))  k.pop_back();
        while (!v.empty() && isspace((unsigned char)v.front())) v.erase(v.begin());
        auto it = fields.find(k);
        if (it == fields.end()) continue;
        switch (it->second.kind) {
            case SettingRef::BOOL:  *(bool*) it->second.p = (atoi(v.c_str()) != 0); break;
            case SettingRef::INT:   *(int*)  it->second.p = atoi(v.c_str());        break;
            case SettingRef::FLOAT: *(float*)it->second.p = (float)atof(v.c_str()); break;
        }
        n++;
    }
    printf("[settings] loaded %d value(s) from %s\n", n, path);
    return n > 0;
}

inline bool saveSettings(const char* path = nullptr) {
    if (!path) path = settingsPath().c_str();
    std::ofstream f(path);
    if (!f) return false;
    f << "# TheFinals menu settings. Delete this file to go back to defaults.\n";
    for (auto& kv : settingFields()) {
        char buf[128];
        switch (kv.second.kind) {
            case SettingRef::BOOL:
                snprintf(buf, sizeof(buf), "%-16s = %d\n", kv.first.c_str(),
                         *(bool*)kv.second.p ? 1 : 0); break;
            case SettingRef::INT:
                snprintf(buf, sizeof(buf), "%-16s = %d\n", kv.first.c_str(),
                         *(int*)kv.second.p); break;
            case SettingRef::FLOAT:
                snprintf(buf, sizeof(buf), "%-16s = %.3f\n", kv.first.c_str(),
                         *(float*)kv.second.p); break;
        }
        f << buf;
    }
    printf("[settings] saved to %s\n", path);
    return true;
}
