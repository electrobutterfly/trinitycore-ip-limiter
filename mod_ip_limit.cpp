/*
 * This file is part of the IP Limiter Module subsystem for TrinityCore 3.3.5.
 * File: src/server/scripts/Custom/IPLimiter/mod_ip_limit.cpp
 *
 * Copyright (C) 2026 Klaus Simon
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldPacket.h"
#include "ObjectAccessor.h"
#include "Config.h"
#include "Log.h"

#include <algorithm>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // Login time (unix epoch) of every currently logged-in player, keyed by
    // the raw GUID. Populated by IPLimit_PlayerScript. Used to determine
    // which players from an over-limit IP are the "newest".
    std::unordered_map<uint64, time_t> g_playerLoginTimes;
}

class IPLimit_PlayerScript : public PlayerScript
{
public:
    IPLimit_PlayerScript() : PlayerScript("IPLimit_PlayerScript") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        g_playerLoginTimes[player->GetGUID().GetRawValue()] = time(nullptr);
    }

    void OnLogout(Player* player) override
    {
        g_playerLoginTimes.erase(player->GetGUID().GetRawValue());
    }
};

class IPLimitModule : public WorldScript
{
public:
    IPLimitModule() : WorldScript("IPLimitModule") { }

    void OnUpdate(uint32 diff) override
    {
        if (!sWorld->getIntConfig(CONFIG_IP_LIMIT_ENABLED))
        {
            if (!_pendingKicks.empty())
                _pendingKicks.clear();
            return;
        }

        // Fire any pending delayed kicks first — independent of the check interval.
        ProcessPendingKicks();

        _updateTimer += diff;

        uint32 checkInterval = sWorld->getIntConfig(CONFIG_IP_LIMIT_CHECK_INTERVAL);
        if (_updateTimer < checkInterval)
            return;

        _updateTimer = 0;

        uint32 maxPlayersPerIP = sWorld->getIntConfig(CONFIG_IP_LIMIT_MAX_PLAYERS);

        // NOTE: this branch of TrinityCore predates ConfigMgr::GetOption<T>, so we
        // use the typed helpers instead. Behaviour is identical.
        int  rawKickMode  = sConfigMgr->GetIntDefault("IPLimit.KickMode", 0);
        int  rawKickDelay = sConfigMgr->GetIntDefault("IPLimit.KickDelay", 0);
        bool showWarning  = sConfigMgr->GetBoolDefault("IPLimit.ShowWarning", true);

        uint32 kickMode  = static_cast<uint32>(std::max(rawKickMode, 0));
        uint32 kickDelay = static_cast<uint32>(std::min(std::max(rawKickDelay, 0), 300));

        TC_LOG_INFO("modules.iplimit",
            "IP Limit check tick -- maxPerIP={} kickMode={} kickDelay={}s",
            maxPlayersPerIP, kickMode, kickDelay);

        std::unordered_map<std::string, std::vector<Player*>> ipPlayerMap;

        SessionMap const& sessions = sWorld->GetAllSessions();

        for (auto const& sessionPair : sessions)
        {
            WorldSession* session = sessionPair.second;
            if (!session)
                continue;

            Player* player = session->GetPlayer();
            if (!player)
                continue;

            std::string ip = session->GetRemoteAddress();

            if (sWorld->IsIPWhitelisted(ip))
            {
                TC_LOG_INFO("modules.iplimit",
                    "Skipping player '{}' from whitelisted IP {}",
                    player->GetName(), ip);
                continue;
            }

            ipPlayerMap[ip].push_back(player);
        }

        for (auto const& ipGroup : ipPlayerMap)
        {
            std::string const& ip = ipGroup.first;
            std::vector<Player*> const& players = ipGroup.second;

            TC_LOG_INFO("modules.iplimit",
                "IP {} has {} session(s) (limit {})",
                ip, players.size(), maxPlayersPerIP);

            if (players.size() <= maxPlayersPerIP)
                continue;

            // Decide who gets kicked.
            std::vector<Player*> toKick;

            if (kickMode == 0)
            {
                // Kick everyone from this IP.
                toKick = players;
            }
            else
            {
                // Kick only the (N - limit) newest sessions, leaving the
                // older ones online. Sort by login time ascending so the
                // tail of the vector is the newest.
                std::vector<Player*> sorted = players;
                std::sort(sorted.begin(), sorted.end(),
                    [](Player* a, Player* b)
                    {
                        auto aIt = g_playerLoginTimes.find(a->GetGUID().GetRawValue());
                        auto bIt = g_playerLoginTimes.find(b->GetGUID().GetRawValue());
                        time_t aTime = (aIt != g_playerLoginTimes.end()) ? aIt->second : 0;
                        time_t bTime = (bIt != g_playerLoginTimes.end()) ? bIt->second : 0;
                        return aTime < bTime;
                    });

                size_t numToKick = sorted.size() - maxPlayersPerIP;
                for (size_t i = sorted.size() - numToKick; i < sorted.size(); ++i)
                    toKick.push_back(sorted[i]);
            }

            std::string kickReason =
                "IP limit exceeded: " + std::to_string(maxPlayersPerIP) +
                " players max per IP";

            for (Player* player : toKick)
            {
                if (!player)
                    continue;

                ObjectGuid guid = player->GetGUID();

                if (_pendingKicks.count(guid.GetRawValue()))
                {
                    TC_LOG_INFO("modules.iplimit",
                        "{} ({}) already scheduled for kick, skipping",
                        player->GetName(), guid.ToString());
                    continue;
                }

                if (kickDelay == 0)
                {
                    TC_LOG_INFO("modules.iplimit",
                        "Kicking player {} ({}) immediately",
                        player->GetName(), guid.ToString());
                    player->GetSession()->KickPlayer(kickReason);
                }
                else
                {
                    PendingKick pk;
                    pk.guid      = guid;
                    pk.ip        = ip;
                    auto ltIt    = g_playerLoginTimes.find(guid.GetRawValue());
                    pk.loginTime = (ltIt != g_playerLoginTimes.end()) ? ltIt->second : 0;
                    pk.kickAt    = time(nullptr) + kickDelay;

                    TC_LOG_INFO("modules.iplimit",
                        "Scheduled kick for player {} ({}) in {}s",
                        player->GetName(), guid.ToString(), kickDelay);

                    // Resolve the message NOW (at schedule time) and cache it
                    // in the PendingKick so that a mid-flight config reload
                    // does not change what this player was promised.
                    if (showWarning)
                    {
                        std::string msgTemplate = sConfigMgr->GetStringDefault(
                            "IPLimit.WarningMessage",
                            "Your IP has been detected to be over the configured limit. "
                            "You will be kicked in {seconds} seconds.");

                        pk.warningMessage = FormatWarning(msgTemplate, kickDelay);

                        if (!pk.warningMessage.empty())
                        {
                            WorldPacket data(SMSG_NOTIFICATION, pk.warningMessage.size() + 1);
                            data << pk.warningMessage;
                            player->GetSession()->SendPacket(&data);
                        }
                    }

                    _pendingKicks[guid.GetRawValue()] = pk;
                }
            }
        }
    }

private:
    struct PendingKick
    {
        ObjectGuid  guid;
        std::string ip;
        time_t      loginTime;      // login time when scheduled — used to detect relogs
        time_t      kickAt;
        std::string warningMessage; // pre-formatted, may be empty
    };

    uint32 _updateTimer = 0;
    std::unordered_map<uint64, PendingKick> _pendingKicks;

    void ProcessPendingKicks()
    {
        if (_pendingKicks.empty())
            return;

        time_t now = time(nullptr);
        uint32 maxPlayersPerIP = sWorld->getIntConfig(CONFIG_IP_LIMIT_MAX_PLAYERS);

        for (auto it = _pendingKicks.begin(); it != _pendingKicks.end(); )
        {
            PendingKick const& pk = it->second;

            if (now < pk.kickAt)
            {
                ++it;
                continue;
            }

            if (Player* player = ObjectAccessor::FindPlayer(pk.guid))
            {
                auto ltIt = g_playerLoginTimes.find(pk.guid.GetRawValue());

                // Only kick if this is still the same session (same login time).
                if (ltIt != g_playerLoginTimes.end() && ltIt->second == pk.loginTime)
                {
                    std::string reason =
                        "IP limit exceeded: " + std::to_string(maxPlayersPerIP) +
                        " players max per IP";

                    TC_LOG_INFO("modules.iplimit",
                        "Executing delayed kick for player {} ({})",
                        player->GetName(), pk.guid.ToString());

                    player->GetSession()->KickPlayer(reason);
                }
                else
                {
                    TC_LOG_INFO("modules.iplimit",
                        "Cancelling delayed kick for {} (session changed)",
                        pk.guid.ToString());
                }
            }

            it = _pendingKicks.erase(it);
        }
    }

    // Replaces every occurrence of "{seconds}" in 'message' with 'seconds'.
    // Returns an empty string if the input is empty (treat as "no message").
    static std::string FormatWarning(std::string message, uint32 seconds)
    {
        if (message.empty())
            return message;

        std::string replacement = std::to_string(seconds);
        std::string const token = "{seconds}";

        size_t pos = 0;
        while ((pos = message.find(token, pos)) != std::string::npos)
        {
            message.replace(pos, token.length(), replacement);
            pos += replacement.length();   // skip past the inserted value
        }

        return message;
    }
};

void AddSC_IPLimitModule()
{
    new IPLimitModule();
    new IPLimit_PlayerScript();

    TC_LOG_INFO("modules.iplimit",
        "IP Limit Module loaded successfully. Enabled: {}, Max players per IP: {}, Check interval: {} ms, Kick mode: {}, Kick delay: {}s, Show warning: {}",
        sWorld->getIntConfig(CONFIG_IP_LIMIT_ENABLED),
        sWorld->getIntConfig(CONFIG_IP_LIMIT_MAX_PLAYERS),
        sWorld->getIntConfig(CONFIG_IP_LIMIT_CHECK_INTERVAL),
        sConfigMgr->GetIntDefault("IPLimit.KickMode", 0),
        sConfigMgr->GetIntDefault("IPLimit.KickDelay", 0),
        sConfigMgr->GetBoolDefault("IPLimit.ShowWarning", true));
}
