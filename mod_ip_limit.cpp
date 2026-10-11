/*
 * This file is part of the AFK Kicker Module subsystem for TrinityCore 3.3.5.
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
#include "Log.h"
#include <unordered_map>

class IPLimitModule : public WorldScript
{
public:
    IPLimitModule() : WorldScript("IPLimitModule") { }

    void OnUpdate(uint32 diff) override
    {
        if (!sWorld->getIntConfig(CONFIG_IP_LIMIT_ENABLED))
            return;

        _updateTimer += diff;

        uint32 checkInterval = sWorld->getIntConfig(CONFIG_IP_LIMIT_CHECK_INTERVAL);

        if (_updateTimer >= checkInterval)
        {
            _updateTimer = 0;

            uint32 maxPlayersPerIP = sWorld->getIntConfig(CONFIG_IP_LIMIT_MAX_PLAYERS);

            std::unordered_map<std::string, std::vector<Player*>> ipPlayerMap;

            SessionMap const& sessions = sWorld->GetAllSessions();

            for (auto const& sessionPair : sessions)
            {
                if (Player* player = sessionPair.second->GetPlayer())
                {
                    std::string ip = sessionPair.second->GetRemoteAddress();

                    if (sWorld->IsIPWhitelisted(ip))
                        continue;

                    ipPlayerMap[ip].push_back(player);
                }
            }

            for (auto const& ipGroup : ipPlayerMap)
            {
                std::string const& ip = ipGroup.first;
                std::vector<Player*> const& players = ipGroup.second;

                if (players.size() > maxPlayersPerIP)
                {
                    TC_LOG_INFO("modules.iplimit", "IP limit exceeded for {}: {} players online (limit: {}). Kicking all.",
                                ip, players.size(), maxPlayersPerIP);

                    for (Player* player : players)
                    {
                        if (player)
                        {
                            TC_LOG_INFO("modules.iplimit", "Kicking player {} (GUID: {}) for IP limit violation.",
                                        player->GetName(), player->GetGUID().GetCounter());

                            std::string kickReason = "IP limit exceeded: " + std::to_string(maxPlayersPerIP) +
                                                     " players max per IP";
                            player->GetSession()->KickPlayer(kickReason);
                        }
                    }
                }
            }
        }
    }

private:
    uint32 _updateTimer = 0;
};

void AddSC_IPLimitModule()
{
    new IPLimitModule();

    TC_LOG_INFO("modules.iplimit", "IP Limit Module loaded successfully. Enabled: {}, Max players per IP: {}, Check interval: {} ms",
                sWorld->getIntConfig(CONFIG_IP_LIMIT_ENABLED),
                sWorld->getIntConfig(CONFIG_IP_LIMIT_MAX_PLAYERS),
                sWorld->getIntConfig(CONFIG_IP_LIMIT_CHECK_INTERVAL));
}
