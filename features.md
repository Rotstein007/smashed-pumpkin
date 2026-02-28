# Pumpkin ↔ Smashed Pumpkin Interface Analysis

Analysis scope: current workspace code (`smashed-pumpkin/src/*` and `pumpkin-source/*`).

## English

### Already used

#### 1) Process + console interface (stdin/stdout/stderr)
- Pumpkin interface: run as child process, write commands to `stdin`, read output from `stdout/stderr`.
- Current use in Smashed Pumpkin:
  - Start/stop/restart via `pumpkin_server_start()` / `pumpkin_server_stop()`.
  - Send commands via `pumpkin_server_send_command()` (`tps`, `list`, `kick`, `ban`, `pardon`, `op`, `deop`, ...).
  - Mirror live output to UI and session log files.
- Relevant code:
  - `src/server.c`
  - `src/window.c`

#### 2) Command-based telemetry (`tps`, `list`)
- Pumpkin interface:
  - `/tps` (`pumpkin-source/pumpkin/src/command/commands/tps.rs`)
  - `/list` (`pumpkin-source/pumpkin/src/command/commands/list.rs`)
- Current use in Smashed Pumpkin:
  - Periodic polling sends both commands.
  - Regex parsing feeds TPS graph and player presence state.
- Relevant code:
  - `src/window.c` (`update_stats_tick`, `parse_tps_from_line`, `parse_player_list_snapshot_line`)

#### 3) Java status ping (SLP)
- Pumpkin interface: status JSON from `CachedStatus`.
- Current use in Smashed Pumpkin:
  - Internal SLP implementation parses `players.online` + `players.max`.
- Relevant code:
  - `src/window.c` (`query_minecraft_players`)
  - `pumpkin-source/pumpkin/src/server/connection_cache.rs`

#### 4) Direct config file integration (`configuration.toml`)
- Pumpkin interface: `config/configuration.toml` (`BasicConfiguration`).
- Current use in Smashed Pumpkin:
  - Before start, writes `java_edition_address`, `bedrock_edition_address`, `max_players`.
- Relevant code:
  - `src/server.c` (`sync_pumpkin_basic_configuration`)

#### 5) Direct data file integration (`data/*.json`)
- Pumpkin interface: `ops.json`, `whitelist.json`, `banned-players.json`, `usercache.json`.
- Current use in Smashed Pumpkin:
  - Build OP/admin tags, whitelist state, ban state/reason, UUID↔name mapping.
- Relevant code:
  - `src/window.c` (`resolve_data_file`, `load_player_entries_from_file`, `refresh_*_list`)

#### 6) Disk-based player history sources
- Pumpkin interface: `data/players`, `world/playerdata`.
- Current use in Smashed Pumpkin:
  - Heuristic first-seen/last-seen fallback + custom persistence (`player-tracking.ini`).
- Relevant code:
  - `src/window.c` (`ingest_players_from_disk`, `player_states_load/save`)

#### 7) Text log stream (including ANSI)
- Pumpkin interface: text logs, often from `to_pretty_console()`.
- Current use in Smashed Pumpkin:
  - ANSI stripping, local timestamp formatting, level extraction, level filters.
- Relevant code:
  - `src/window.c` (`strip_ansi`, `sanitize_console_text`, `format_console_line`)
  - `pumpkin-source/pumpkin/src/logging.rs`

#### 8) Update source (web/release metadata)
- Pumpkin interface: public download/release pages.
- Current use in Smashed Pumpkin:
  - Resolve latest binary URL/build metadata via libsoup + fallback.
- Relevant code:
  - `src/download.c`

#### 9) Parent-death child termination (Linux)
- Current use in Smashed Pumpkin:
  - Uses `PR_SET_PDEATHSIG` to stop Pumpkin if app process dies unexpectedly.
- Relevant code:
  - `src/server.c` (`child_setup_cb`)

### Available but not used (or not fully used) yet

#### 1) RCON protocol (TCP, authenticated)
- Available in Pumpkin:
  - `pumpkin-source/pumpkin/src/net/rcon/mod.rs`
  - `pumpkin-config/src/networking/rcon.rs`
- Value:
  - Robust command/response channel decoupled from console text parsing.

#### 2) Legacy Query Full-Status (UDP)
- Available in Pumpkin:
  - `pumpkin-source/pumpkin/src/net/query.rs`
- Value:
  - Returns MOTD, map, plugins, `num_players`, `max_players`.
- Important assessment:
  - Yes, this is very good for `online/max` counts.
  - It is not enough for full player list UX because names in full query are intentionally capped to 4.

#### 3) Status sample list (UUID + name)
- Available in Pumpkin:
  - `CachedStatus.player_samples` (up to 12 entries).
- Value:
  - Useful for head/UUID mapping of currently visible sample users.
- Important assessment:
  - Not a complete source for full online list.
  - Also not a guaranteed full-platform source:
    - It is a sample (max 12).
    - Bedrock players are not added through the same `server_listing` path used for Java config-driven listing.
    - Java users can opt out of server listing.

#### 4) `tick query` command (percentiles)
- Available in Pumpkin:
  - `pumpkin-source/pumpkin/src/command/commands/tick.rs`
- Value:
  - Adds P50/P95/P99 visibility for performance diagnostics.

#### 5) Plugin API/events
- Available in Pumpkin:
  - `pumpkin-source/pumpkin/src/plugin/api/events/*`
- Value:
  - Cleaner future integration path than parsing free-form logs.

#### 6) Runtime plugin management commands
- Available in Pumpkin:
  - `command/commands/plugin.rs`, `plugins.rs`
- Value:
  - Runtime loaded/active state, not only filesystem state.

#### 7) `ban-ip` / `pardon-ip` / `banlist`
- Available in Pumpkin:
  - `command/commands/banip.rs`, `pardonip.rs`, `banlist.rs`
- Value:
  - High value for moderation workflows.
  - Recommended to integrate in Smashed Pumpkin as first-class actions next to normal bans.

#### 8) LAN broadcast
- Available in Pumpkin:
  - `pumpkin-source/pumpkin/src/net/lan_broadcast.rs`
- Value:
  - Optional local-discovery/status feature potential.

#### 9) Proxy integration (Velocity/Bungee) + auth profile handling
- Available in Pumpkin:
  - `pumpkin-config/src/networking/proxy.rs`, `pumpkin/src/net/proxy/*`, `networking/auth.rs`
- Important assessment for Java/Bedrock detection:
  - Useful in proxy setups, but not a universal platform detector for Smashed Pumpkin.
  - It does not currently provide a stable external “player platform” API for the app.
  - Can support better identity/address handling in proxied environments, but does not replace a dedicated platform field/event.

### Features currently solved without a clean interface

| Feature | Current workaround in Smashed Pumpkin | Main issue | Needed clean interface |
|---|---|---|---|
| Per-player Java/Bedrock detection | Log heuristics + IP hints + UUID pattern guesses | fragile, depends on log shape/verbosity | structured player event/status with explicit `platform` |
| Complete player history (first seen, last online, playtime) | merge of json files + directory scans + custom `player-tracking.ini` | heuristic inconsistencies | official persistent player registry export/API |
| Offline moderation for known players | UI knows players from files, command path still online-oriented in many commands | offline targeting is inconsistent | command variants that accept offline name/UUID directly |
| Stable online/max counters | mixed SLP + `list` + local presence state | source drift/fallback complexity | single machine-readable status endpoint |
| Console styling/filtering | strip ANSI + parse free text | brittle on upstream format changes | structured logging mode (e.g. JSON) |
| Plugin metadata accuracy | file/filename heuristics | runtime state can differ | runtime plugin status endpoint |
| Player heads/skins | external Crafatar fetch by UUID | external dependency + platform edge cases | direct validated texture info API |
| Update detection | HTML/release scraping | scraping fragility | stable versioned release metadata API |

### Practical recommendation
- For player counts now:
  - Prefer **Query Full-Status** (if enabled) for `online/max`.
  - Keep SLP and local tracking as fallback.
- For player roster now:
  - Keep `list` + local state until Pumpkin exposes a complete machine-readable player list.
- For moderation next:
  - Integrate `ban-ip` / `pardon-ip` / `banlist` as native UI operations.
- For platform detection:
  - Proxy/auth modules help in specific deployments, but still add a dedicated server-side platform field/event for reliability.

---

## Deutsch

### Bereits genutzt

#### 1) Prozess- und Konsolen-Schnittstelle (stdin/stdout/stderr)
- Pumpkin-Schnittstelle: Child-Process starten, Befehle über `stdin`, Ausgaben über `stdout/stderr`.
- Aktuelle Nutzung in Smashed Pumpkin:
  - Start/Stop/Restart über `pumpkin_server_start()` / `pumpkin_server_stop()`.
  - Befehle über `pumpkin_server_send_command()` (`tps`, `list`, `kick`, `ban`, `pardon`, `op`, `deop`, ...).
  - Live-Ausgabe in UI und Session-Logdateien.
- Relevanter Code:
  - `src/server.c`
  - `src/window.c`

#### 2) Command-basierte Telemetrie (`tps`, `list`)
- Pumpkin-Schnittstelle:
  - `/tps` (`pumpkin-source/pumpkin/src/command/commands/tps.rs`)
  - `/list` (`pumpkin-source/pumpkin/src/command/commands/list.rs`)
- Aktuelle Nutzung in Smashed Pumpkin:
  - Zyklisches Polling sendet beide Commands.
  - Regex-Parsing speist TPS-Graph und Spielerstatus.
- Relevanter Code:
  - `src/window.c` (`update_stats_tick`, `parse_tps_from_line`, `parse_player_list_snapshot_line`)

#### 3) Java Status Ping (SLP)
- Pumpkin-Schnittstelle: Status-JSON aus `CachedStatus`.
- Aktuelle Nutzung in Smashed Pumpkin:
  - Eigene SLP-Implementierung liest `players.online` + `players.max`.
- Relevanter Code:
  - `src/window.c` (`query_minecraft_players`)
  - `pumpkin-source/pumpkin/src/server/connection_cache.rs`

#### 4) Direkte Konfig-Datei-Integration (`configuration.toml`)
- Pumpkin-Schnittstelle: `config/configuration.toml` (`BasicConfiguration`).
- Aktuelle Nutzung in Smashed Pumpkin:
  - Vor Start werden `java_edition_address`, `bedrock_edition_address`, `max_players` geschrieben.
- Relevanter Code:
  - `src/server.c` (`sync_pumpkin_basic_configuration`)

#### 5) Direkte Daten-Datei-Integration (`data/*.json`)
- Pumpkin-Schnittstelle: `ops.json`, `whitelist.json`, `banned-players.json`, `usercache.json`.
- Aktuelle Nutzung in Smashed Pumpkin:
  - OP/Admin-Tags, Whitelist-Status, Ban-Status/-Grund, UUID↔Name-Mapping.
- Relevanter Code:
  - `src/window.c` (`resolve_data_file`, `load_player_entries_from_file`, `refresh_*_list`)

#### 6) Disk-basierte Spielerhistorie
- Pumpkin-Schnittstelle: `data/players`, `world/playerdata`.
- Aktuelle Nutzung in Smashed Pumpkin:
  - Heuristische First/Last-Seen-Fallbacks + eigene Persistenz (`player-tracking.ini`).
- Relevanter Code:
  - `src/window.c` (`ingest_players_from_disk`, `player_states_load/save`)

#### 7) Text-Logstream (inkl. ANSI)
- Pumpkin-Schnittstelle: textuelle Logs, oft via `to_pretty_console()`.
- Aktuelle Nutzung in Smashed Pumpkin:
  - ANSI entfernen, Zeit lokal formatieren, Level extrahieren, Filter anwenden.
- Relevanter Code:
  - `src/window.c` (`strip_ansi`, `sanitize_console_text`, `format_console_line`)
  - `pumpkin-source/pumpkin/src/logging.rs`

#### 8) Update-Quelle (Web/Release-Metadaten)
- Pumpkin-Schnittstelle: öffentliche Download-/Release-Seiten.
- Aktuelle Nutzung in Smashed Pumpkin:
  - Neueste Binary-URL + Build-Metadaten via libsoup + Fallback.
- Relevanter Code:
  - `src/download.c`

#### 9) Parent-Death-Absicherung (Linux)
- Aktuelle Nutzung in Smashed Pumpkin:
  - `PR_SET_PDEATHSIG`, damit Pumpkin bei unerwartetem App-Ende beendet wird.
- Relevanter Code:
  - `src/server.c` (`child_setup_cb`)

### Vorhanden, aber noch nicht (oder nicht vollständig) genutzt

#### 1) RCON-Protokoll (TCP, authentifiziert)
- Vorhanden in Pumpkin:
  - `pumpkin-source/pumpkin/src/net/rcon/mod.rs`
  - `pumpkin-config/src/networking/rcon.rs`
- Mehrwert:
  - Robuster Command/Response-Kanal ohne Abhängigkeit vom Console-Text.

#### 2) Legacy Query Full-Status (UDP)
- Vorhanden in Pumpkin:
  - `pumpkin-source/pumpkin/src/net/query.rs`
- Mehrwert:
  - Liefert MOTD, Map, Plugins, `num_players`, `max_players`.
- Wichtige Bewertung:
  - Ja, sehr gut für `online/max`-Werte.
  - Nicht ausreichend für vollständige Spielerliste, weil Namen absichtlich auf 4 begrenzt sind.

#### 3) Status-Sample-Liste (UUID + Name)
- Vorhanden in Pumpkin:
  - `CachedStatus.player_samples` (bis 12 Einträge).
- Mehrwert:
  - Nützlich für Head/UUID-Mapping bei sichtbaren Sample-Spielern.
- Wichtige Bewertung:
  - Keine vollständige Quelle für komplette Online-Liste.
  - Auch keine garantiert vollständige Plattformquelle:
    - nur Sample (max. 12),
    - Bedrock wird nicht über denselben `server_listing`-Pfad wie Java eingespeist,
    - Java-Spieler können Listing deaktivieren.

#### 4) `tick query`-Command (Perzentile)
- Vorhanden in Pumpkin:
  - `pumpkin-source/pumpkin/src/command/commands/tick.rs`
- Mehrwert:
  - P50/P95/P99 für präzisere Performance-Diagnose.

#### 5) Plugin-API/Eventsystem
- Vorhanden in Pumpkin:
  - `pumpkin-source/pumpkin/src/plugin/api/events/*`
- Mehrwert:
  - Sauberere Integrationsbasis als freie Log-Textanalyse.

#### 6) Runtime-Plugin-Commands
- Vorhanden in Pumpkin:
  - `command/commands/plugin.rs`, `plugins.rs`
- Mehrwert:
  - Runtime-Status (geladen/aktiv), nicht nur Dateisystemsicht.

#### 7) `ban-ip` / `pardon-ip` / `banlist`
- Vorhanden in Pumpkin:
  - `command/commands/banip.rs`, `pardonip.rs`, `banlist.rs`
- Mehrwert:
  - Hoher Mehrwert für Moderation.
  - Empfohlen als native UI-Aktionen zusätzlich zu normalen Bans.

#### 8) LAN Broadcast
- Vorhanden in Pumpkin:
  - `pumpkin-source/pumpkin/src/net/lan_broadcast.rs`
- Mehrwert:
  - Optional für lokale Discovery-/Status-Features.

#### 9) Proxy-Integration (Velocity/Bungee) + Auth-Profile
- Vorhanden in Pumpkin:
  - `pumpkin-config/src/networking/proxy.rs`, `pumpkin/src/net/proxy/*`, `networking/auth.rs`
- Wichtige Bewertung für Java/Bedrock-Erkennung:
  - Hilfreich in Proxy-Setups, aber kein universeller Plattformdetektor für Smashed Pumpkin.
  - Aktuell kein stabiler externer „player platform“-API-Wert für die App.
  - Verbessert Identität/Adresse in proxied Umgebungen, ersetzt aber kein dediziertes Plattformfeld/Event.

### Features, die aktuell ohne saubere Schnittstelle gelöst werden

| Feature | Aktueller Workaround in Smashed Pumpkin | Hauptproblem | Benötigte saubere Schnittstelle |
|---|---|---|---|
| Java/Bedrock-Erkennung pro Spieler | Log-Heuristik + IP-Hints + UUID-Muster | fragil, abhängig von Logform/Verbosity | strukturierter Player-Event/Status mit explizitem `platform` |
| Vollständige Spielerhistorie (first seen, last online, playtime) | Mix aus JSON-Dateien + Verzeichnisscan + `player-tracking.ini` | heuristische Inkonsistenzen | offizieller persistenter Player-Registry-Export/API |
| Offline-Moderation für bekannte Spieler | UI kennt Spieler aus Dateien, Command-Pfad oft online-orientiert | offline Targeting inkonsistent | Command-Varianten mit direktem Offline-Name/UUID |
| Stabiler Online/Max-Playercounter | Mix aus SLP + `list` + lokalem Presence-State | Quellen driften/Fallback-Komplexität | ein maschinenlesbarer Status-Endpunkt |
| Console-Styling/Filter | ANSI strippen + freien Text parsen | bricht bei Upstream-Formatänderungen | strukturierter Logging-Mode (z. B. JSON) |
| Plugin-Metadaten-Genauigkeit | Dateisystem-/Dateinamensheuristik | Runtime-Zustand kann abweichen | Runtime-Plugin-Status-Endpunkt |
| Player-Heads/Skins | externer Crafatar-Fetch per UUID | externe Abhängigkeit + Plattformkanten | direkte validierte Texture-Info-API |
| Update-Erkennung | HTML-/Release-Scraping | Scraping-fragil | stabile versionierte Release-Metadaten-API |

### Praktische Empfehlung
- Für Spieleranzahl jetzt:
  - **Query Full-Status** bevorzugen (wenn aktiv) für `online/max`.
  - SLP + lokales Tracking als Fallback behalten.
- Für Spielerliste jetzt:
  - `list` + lokalen State nutzen, bis Pumpkin eine vollständige maschinenlesbare Player-Liste anbietet.
- Für Moderation als nächstes:
  - `ban-ip` / `pardon-ip` / `banlist` als native UI-Funktionen integrieren.
- Für Plattform-Erkennung:
  - Proxy/Auth hilft in bestimmten Deployments, aber für Zuverlässigkeit ein dediziertes Plattformfeld/Event serverseitig ergänzen.
