# trinitycore-ip-limiter

![banner](banner.jpg)

A module for [TrinityCore 3.3.5](https://www.trinitycore.org/) that limits how many players may connect from a single IP address. When the limit is exceeded, players from that IP are kicked according to `IPLimit.KickMode` — either all of them (default), or only the newest connections. Specific IP addresses (e.g. internet cafés, LAN parties, or your own office) can be whitelisted and exempted from the limit.

This is useful for reducing multi-boxing abuse, preventing a single host from flooding the realm, and keeping player slots available for legitimate players.

> **Core modifications are required.** This module reads its settings through TrinityCore's `World` singleton, so it needs a small patch to `World.h` and `World.cpp` before it will compile. See the [Installation](#installation) section for the exact changes.

---

Not running TrinityCore? Check out the [port for AzerothCore-wotlk](https://github.com/electrobutterfly/mod-ip-limiter)

------



## What It Does

While the world server is running, this module periodically scans all connected sessions:

1. Every `IPLimit.CheckInterval` milliseconds, it groups all active sessions by their remote IP address.
2. Sessions from IPs on the whitelist are ignored completely.
3. For every non-whitelisted IP, it counts the number of connected players.
4. If the count **exceeds** `IPLimit.MaxPlayersPerIP`, sessions from that IP are kicked according to `IPLimit.KickMode`:
   - `KickMode = 0`: **all** sessions from that IP are kicked.
   - `KickMode = 1`: only the **newest** `(count - limit)` sessions are kicked; the older ones stay online.

   If `IPLimit.KickDelay > 0`, the kick is deferred and the affected players receive a middle‑screen warning first (see `IPLimit.ShowWarning` / `IPLimit.WarningMessage`). The kick reason shown in the disconnect message is `IP limit exceeded: N players max per IP`.
5. The whitelist is reloaded whenever the configuration is reloaded (`.reload config`).

Note that the check is a periodic scan, not a real-time rejection at login. A burst of connections may briefly exceed the limit until the next check tick. With `IPLimit.KickDelay > 0`, the actual kick fires up to `CheckInterval + KickDelay` after the violation first appears.

---

## Installation

Clone the module into your TrinityCore `/src/server/scripts/Custom/` directory (from the root of your TrinityCore source tree):

```bash
cd src/server/scripts/Custom/
git clone https://github.com/electrobutterfly/trinitycore-ip-limiter.git
```

### Step 1 — Patch `World.h`

Open `src/server/game/World/World.h`. This file defines the `World` class, which is TrinityCore's central configuration holder. Three small additions are needed. Each one uses **Search + Insert relative to a specific anchor line** — the anchor is a line that is present in every TrinityCore 3.3.5 branch, so you don't have to worry about surrounding code that varies between versions.

> **Use your editor's Search function** (Ctrl+F / Cmd+F) to jump to each anchor. Do not skip any of the three additions — the module will not compile if any one is missing.

---

#### 1a. Add three enum values to `WorldIntConfigs`

The `WorldIntConfigs` enum is a numbered list. Each number is an index into an internal array. New entries must be added **at the end**, right before `INT_CONFIG_VALUE_COUNT`. Inserting them anywhere else shifts every subsequent number and silently scrambles your server's settings — this is a very common mistake.

**Search for this line** (near the bottom of the `WorldIntConfigs` enum):

```cpp
    INT_CONFIG_VALUE_COUNT
```

**Insert the three new lines just above it.** So that `INT_CONFIG_VALUE_COUNT` becomes:

```cpp
    // ADDED FOR IP LIMIT MODULE
    CONFIG_IP_LIMIT_ENABLED,
    CONFIG_IP_LIMIT_MAX_PLAYERS,
    CONFIG_IP_LIMIT_CHECK_INTERVAL,
    // END IP LIMIT MODULE
    INT_CONFIG_VALUE_COUNT
```

> **Critical:** the three entries must be **above** `INT_CONFIG_VALUE_COUNT` and **below** whatever entry currently sits right above it (the last real config entry). The `// ADDED FOR ...` and `// END ...` comment lines are optional but recommended — they make future upgrades much easier.

---

#### 1b. Add the whitelist container

The `World` class stores everything about the server. We need to add one new private storage field — a list of whitelisted IP addresses.

The `World` class ends with a `friend` declaration followed by a closing `};`. That `friend` line is your anchor.

**Search for this line** (it's the very last declaration inside the `class World { ... }` block):

```cpp
    friend class debug_commandscript;
```

**Insert the new member directly above it**, with a blank line on each side. Your file should look like this when you're done:

```cpp
        time_t _warnShutdownTime;

        // IP Limit Module whitelist
        std::vector<std::string> m_ipWhitelist;

    friend class debug_commandscript;
};
```

**Key points:**

- The new line is indented with **8 spaces** (to align with `time_t _warnShutdownTime;` and the other private members above it).
- It goes **after** the last existing private member (whatever that is in your branch — `_warnShutdownTime`, or `_guidAlertLock`, or something else) and **before** `friend class debug_commandscript;`.
- If your branch does not have `_warnShutdownTime` (very old fork), the anchor is still the same: insert just above `friend class debug_commandscript;`.

---

#### 1c. Declare two helper methods

The `World` class needs two public method declarations so the module can ask "is this IP whitelisted?" and "how many players come from this IP?".

The anchor for this is the transition from the **public** section to the **protected** section of the class. This transition is present in every branch and always begins with:

**Search for this line** (it appears roughly in the middle of the class):

```cpp
    protected:
```

Then look one line below it — you should see `void _UpdateGameTime();`. The full anchor is:

```cpp
    protected:
        void _UpdateGameTime();
```

**Insert the two new declarations directly above `protected:`**, with a blank line on each side. Your file should look like this when you're done:

```cpp
        // IP Limit Module helper methods
        uint32 GetPlayerCountByIP(std::string const& ip);
        bool IsIPWhitelisted(std::string const& ip);

    protected:
        void _UpdateGameTime();
```

**Key points:**

- The new lines are indented with **8 spaces** (to align with the public members above them).
- `protected:` must remain **below** the new lines. Do not delete or move it.
- Do not insert the new lines **inside** the protected section — that would make them protected instead of public, and the module couldn't call them.

> **Note on `GetPlayerCountByIP`:** this method is kept for backward compatibility with earlier versions of the module. The current module does not call it — you may omit it if you prefer a minimal patch. `IsIPWhitelisted` is **required**.

---

#### Sanity check

After all three edits, save the file. Then run this search across the entire file to confirm each change is present exactly once:

```bash
grep -c "CONFIG_IP_LIMIT_ENABLED"    src/server/game/World/World.h   # expect: 1
grep -c "m_ipWhitelist"              src/server/game/World/World.h   # expect: 1
grep -c "IsIPWhitelisted"            src/server/game/World/World.h   # expect: 1
```

If any count is `0`, that edit is missing — go back and re-check. If any count is `2` or more, you accidentally pasted twice; delete the duplicate.

Now proceed to Step 2.

### Step 2 — Patch `World.cpp`

Open `src/server/game/World.cpp` and make **two** additions.

**2a. Add the config load block.** Search for:

```cpp
m_bool_configs[CONFIG_PREVENT_RENAME_CUSTOMIZATION] = sConfigMgr->GetBoolDefault("PreventRenameCharacterOnCustomization", false);
```

Paste this block **just below** it:

```cpp
// Load IP LIMIT module settings
m_int_configs[CONFIG_IP_LIMIT_ENABLED] = sConfigMgr->GetIntDefault("IPLimit.Enabled", 0);
m_int_configs[CONFIG_IP_LIMIT_MAX_PLAYERS] = sConfigMgr->GetIntDefault("IPLimit.MaxPlayersPerIP", 5);
m_int_configs[CONFIG_IP_LIMIT_CHECK_INTERVAL] = sConfigMgr->GetIntDefault("IPLimit.CheckInterval", 30000);

// Load and parse the IP whitelist
std::string whitelistStr = sConfigMgr->GetStringDefault("IPLimit.Whitelist", "");
m_ipWhitelist.clear();
if (!whitelistStr.empty())
{
    std::string token;
    std::stringstream ss(whitelistStr);
    while (std::getline(ss, token, ','))
    {
        size_t start = token.find_first_not_of(" \t");
        size_t end = token.find_last_not_of(" \t");
        if (start != std::string::npos && end != std::string::npos)
            m_ipWhitelist.push_back(token.substr(start, end - start + 1));
    }
}
TC_LOG_INFO("server.loading", "Loaded IP limit whitelist with {} entries", m_ipWhitelist.size());
```

> Ensure `#include <sstream>` is present in `World.cpp` (it almost certainly already is for other config parsing).

**2b. Add the method implementations.** Anywhere in `World.cpp` after the class methods (for example, near `World::LoadWorldStates()` or at the end of the file before other free functions), add:

```cpp
bool World::IsIPWhitelisted(std::string const& ip)
{
    return std::find(m_ipWhitelist.begin(), m_ipWhitelist.end(), ip) != m_ipWhitelist.end();
}

uint32 World::GetPlayerCountByIP(std::string const& ip)
{
    uint32 count = 0;
    for (auto const& sessionPair : m_sessions)
    {
        WorldSession* session = sessionPair.second;
        if (!session)
            continue;
        if (!session->GetPlayer())
            continue;
        if (session->GetRemoteAddress() == ip)
            ++count;
    }
    return count;
}
```

> Only the `IsIPWhitelisted` implementation is required. `GetPlayerCountByIP` is optional (see note in step 1c). `std::find` requires `#include <algorithm>` — verify it is present at the top of `World.cpp`.


### Step 3 — Rebuild

```bash
cd ..
mkdir -p build
cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/path/to/your/server
make -j$(nproc)
make install
```

### Step 4 — Configure

After `make install`, **copy the contents of `mod_ip_limiter.conf` into the `worldserver.conf`** of your server installation. (TrinityCore does not auto-load per-module config files the way AzerothCore does; all `IPLimit.*` keys must live inside `worldserver.conf`.)

Restart the worldserver to load the module.

---

## Configuration

All options live in `worldserver.conf` (copy them there from `mod_ip_limiter.conf`).

### `IPLimit.Enabled`

Enable or disable the IP limiter module.

| Value | Meaning  |
| :---- | :------- |
| `1`   | Enabled  |
| `0`   | Disabled |

Default: `0` (disabled)

> The module ships disabled by default. Set this to `1` after you have verified your `MaxPlayersPerIP` and whitelist settings.

### `IPLimit.MaxPlayersPerIP`

Maximum number of simultaneous players allowed from a single IP address. If the count **exceeds** this value, players from that IP are kicked according to `IPLimit.KickMode`.

Default: `5`

Example: with `MaxPlayersPerIP = 5`, up to 5 players from one IP are fine. The 6th player triggers a kick. In mode 0 all 6 are kicked; in mode 1 only the newest one is kicked.

### `IPLimit.CheckInterval`

How often (in milliseconds) the module scans sessions for IP limit violations. Lower values react faster to violations but use slightly more CPU; higher values are lighter but allow brief over-limit periods.

Default: `30000` (30 seconds)

### `IPLimit.Whitelist`

A comma-separated list of IP addresses that are exempt from the limit. Whitespace around entries is trimmed.

Default: `""` (empty — no IPs whitelisted)

Example:

```bash
IPLimit.Whitelist = "192.168.1.100,192.168.1.101,203.0.113.50"
```

### `IPLimit.KickMode`

What to do when the per-IP limit is exceeded.

| Value | Behaviour |
| :---- | :-------- |
| `0`   | Kick **all** players from the offending IP. |
| `1`   | Kick only the `(count - limit)` **newest** players, leaving the older ones online. |

Default: `0` (kick all, preserves legacy behaviour).

"Newest" is determined by login time (tracked internally by the module), **not** by character GUID, account ID, or character creation date.

### `IPLimit.KickDelay`

Delay, in seconds, before the actual kick is performed. Hard cap: `300` (5 minutes).

| Value | Behaviour |
| :---- | :-------- |
| `0`   | Kick immediately (no warning). |
| `1..300` | Schedule the kick; the player(s) receive a warning first (see `IPLimit.ShowWarning`). |

Default: `0`.

Cancellation rules for pending kicks:

- If a scheduled player logs off before the timer fires, no kick is performed.
- If a scheduled player relogs before the timer fires, the pending kick is cancelled — a fresh login starts with a clean slate.
- Disabling the module (`IPLimit.Enabled = 0`) cancels all pending kicks.

### `IPLimit.ShowWarning`

When `IPLimit.KickDelay > 0`, send a middle‑screen notification to each player who is scheduled for a kick. Only affects players who are **actually** being kicked in the current tick — in `KickMode = 0` that is every player from the IP; in `KickMode = 1` only the newest ones receive the warning.

| Value | Behaviour |
| :---- | :-------- |
| `1`   | Warning enabled (default). |
| `0`   | Warning disabled — kick happens silently. |

### `IPLimit.WarningMessage`

The text of the middle‑screen notification sent to each player who is scheduled for a delayed kick. Only used when `IPLimit.KickDelay > 0` **and** `IPLimit.ShowWarning = 1`.

The literal token `{seconds}` is replaced with the configured `IPLimit.KickDelay` value. All occurrences are replaced. If the token is absent, the message is sent unchanged.

Example message (default):

> Your IP has been detected to be over the configured limit. You will be kicked in 30 seconds.

| Value | Behaviour |
| :---- | :-------- |
| Non‑empty string | Sent verbatim, with `{seconds}` substituted. |
| `""` (empty string) | No message is sent. Equivalent to `IPLimit.ShowWarning = 0`. |

Default:

```bash
IPLimit.WarningMessage = "Your IP has been detected to be over the configured limit. You will be kicked in {seconds} seconds."
```

### Example

```bash
IPLimit.Enabled          = 1
IPLimit.MaxPlayersPerIP  = 3
IPLimit.CheckInterval    = 30000
IPLimit.Whitelist        = "203.0.113.10,203.0.113.11"
IPLimit.KickMode         = 1
IPLimit.KickDelay        = 30
IPLimit.ShowWarning      = 1
IPLimit.WarningMessage   = "Too many players from your network. You have {seconds}s to wrap up before being disconnected."
```

------

## Logging

The module writes log lines about each tick, each non-whitelisted IP's session count, whitelist skips, each scheduled kick, and each kick actually performed.

Most lines go through the dedicated `modules.iplimit` logger, which the shipped configuration file declares:

```bash
Logger.modules.iplimit = 3,Console Server
```

- `3` is the log level (see TrinityCore logging levels).
- `Console Server` means output goes to both the console and the server log file.

One line — the whitelist-loaded message at startup — is emitted through the `server.loading` logger because it runs from `World::LoadConfigSettings()` inside your patched `World.cpp`. If you want to silence **that** one line specifically, lower the `server.loading` logger's level in `worldserver.conf`. All other module log lines use `modules.iplimit`.

------

## Notes

- Only connected sessions with a valid `Player` object are counted. Sessions still at character selection or in the login process are ignored.
- Grouping is done purely by IP string, so two players behind the same NAT/router share a bucket. Use the whitelist for known shared connections (e.g. internet cafés, dorms, offices).
- The limit is inclusive up to `MaxPlayersPerIP`; the kick triggers only when the count is strictly greater.
- Because checks are periodic, raising `MaxPlayersPerIP` during peak hours takes effect at the next tick.
- Configuration reload (e.g. `.reload config`) re-reads the whitelist immediately (via `World::LoadConfigSettings()`) and the four newer options (`KickMode`, `KickDelay`, `ShowWarning`, `WarningMessage`) at the next check tick. A warning message that has already been shown is not changed retroactively — the scheduled kick keeps the wording the player was promised.
- The four newer options are read directly from `sConfigMgr` on each tick rather than through the `World::getIntConfig` enum. That means adding them to `World.h` / `World.cpp` was **not** necessary and was **not** done — only the three original keys (`Enabled`, `MaxPlayersPerIP`, `CheckInterval`) and `Whitelist` are part of the core patch.

------

## License

This module is licensed under the GNU General Public License v2 (or later). See the header comments in the source files for details.

Copyright (C) 2026 Klaus Simon

<img src="https://electrobutterfly.com/images/logo-small-github.png" alt="Logo" style="float:left; margin-right:10px; width:150px;">
