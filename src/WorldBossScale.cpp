/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Copyright (C) Aldrynth / VenomekPL
 *
 * Outdoor world-boss scaler. Linear from designed raid size down to MinPlayers
 * using a live threat-list recount. Instance maps are ignored (AutoBalance).
 */

#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "Creature.h"
#include "DataMap.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "TemporarySummon.h"
#include "ThreatManager.h"
#include "Tokenize.h"
#include "Unit.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    char const* const InfoKey = "WorldBossScale";

    struct CatalogEntry
    {
        uint32 entry;
        uint8 designed;
        char const* name;
    };

    CatalogEntry const BuiltIn[] =
    {
        { 6109,  40, "Azuregos" },
        { 12397, 40, "Lord Kazzak" },
        { 14887, 40, "Ysondre" },
        { 14888, 40, "Lethon" },
        { 14889, 40, "Emeriss" },
        { 14890, 40, "Taerar" },
        { 17711, 25, "Doomwalker" },
        { 18728, 25, "Doom Lord Kazzak" },
    };

    struct WBSConfig
    {
        bool enable = true;
        bool announce = false;
        uint8 minPlayers = 1;
        float healthMod = 1.0f;
        float damageMod = 1.0f;
        float threatRange = 150.0f;
        uint32 recountMs = 1000;
    };

    WBSConfig cfg;
    std::unordered_map<uint32, uint8> DesignedPlayers;

    class WBSInfo : public DataMap::Base
    {
    public:
        uint32 originalCreateHealth = 0;
        uint32 playerCount = 0;
        uint32 rawPlayerCount = 0;
        uint8 designedPlayers = 0;
        float healthMult = 1.0f;
        float damageMult = 1.0f;
        uint32 recountTimer = 0;
        bool isSummon = false;
        bool originalsCached = false;
    };

    uint8 LookupDesigned(uint32 entry)
    {
        auto const it = DesignedPlayers.find(entry);
        if (it == DesignedPlayers.end())
            return 0;
        return it->second;
    }

    void ParseExtraEntries(std::string raw)
    {
        if (raw.size() >= 2 && ((raw.front() == '"' && raw.back() == '"') || (raw.front() == '\'' && raw.back() == '\'')))
            raw = raw.substr(1, raw.size() - 2);

        for (std::string_view token : Acore::Tokenize(raw, ',', false))
        {
            while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
                token.remove_prefix(1);
            while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
                token.remove_suffix(1);
            if (token.empty())
                continue;

            auto const colon = token.find(':');
            if (colon == std::string_view::npos || colon == 0 || colon + 1 >= token.size())
            {
                LOG_WARN("module", "WorldBossScale: bad ExtraEntries token '{}' — expected entry:size",
                    std::string(token));
                continue;
            }

            uint32 entry = 0;
            uint32 designed = 0;
            try
            {
                entry = static_cast<uint32>(std::stoul(std::string(token.substr(0, colon))));
                designed = static_cast<uint32>(std::stoul(std::string(token.substr(colon + 1))));
            }
            catch (...)
            {
                LOG_WARN("module", "WorldBossScale: unreadable ExtraEntries token '{}'", std::string(token));
                continue;
            }

            if (!entry || designed < 1 || designed > 40)
            {
                LOG_WARN("module", "WorldBossScale: ExtraEntries {} : {} out of range — ignored", entry, designed);
                continue;
            }

            DesignedPlayers[entry] = static_cast<uint8>(designed);
        }
    }

    void LoadConfig()
    {
        cfg.enable = sConfigMgr->GetOption<bool>("WorldBossScale.Enable", true);
        cfg.announce = sConfigMgr->GetOption<bool>("WorldBossScale.Announce", false);
        cfg.minPlayers = sConfigMgr->GetOption<uint8>("WorldBossScale.MinPlayers", 1);
        cfg.healthMod = sConfigMgr->GetOption<float>("WorldBossScale.HealthMod", 1.0f);
        cfg.damageMod = sConfigMgr->GetOption<float>("WorldBossScale.DamageMod", 1.0f);
        cfg.threatRange = sConfigMgr->GetOption<float>("WorldBossScale.ThreatRange", 150.0f);
        cfg.recountMs = sConfigMgr->GetOption<uint32>("WorldBossScale.RecountMs", 1000);

        if (cfg.minPlayers < 1)
            cfg.minPlayers = 1;
        if (cfg.recountMs < 200)
            cfg.recountMs = 200;
        if (cfg.healthMod < 0.0f)
            cfg.healthMod = 0.0f;
        if (cfg.damageMod < 0.0f)
            cfg.damageMod = 0.0f;

        DesignedPlayers.clear();
        for (CatalogEntry const& row : BuiltIn)
            DesignedPlayers[row.entry] = row.designed;

        ParseExtraEntries(sConfigMgr->GetOption<std::string>("WorldBossScale.ExtraEntries", ""));
    }

    WBSInfo* GetInfo(Creature* creature, bool create)
    {
        if (!creature)
            return nullptr;
        if (create)
            return creature->CustomData.GetDefault<WBSInfo>(InfoKey);
        return creature->CustomData.Get<WBSInfo>(InfoKey);
    }

    WBSInfo* GetInfo(Unit* unit)
    {
        if (!unit || !unit->IsCreature())
            return nullptr;
        return GetInfo(unit->ToCreature(), false);
    }

    Creature* GetSummonerCreature(Creature* creature)
    {
        if (!creature || !creature->IsSummon())
            return nullptr;

        TempSummon* summon = creature->ToTempSummon();
        if (!summon)
            return nullptr;

        if (Creature* base = summon->GetSummonerCreatureBase())
            return base;

        if (Unit* unit = summon->GetSummonerUnit())
            return unit->ToCreature();

        return nullptr;
    }

    uint32 CountThreatPlayers(Creature* creature)
    {
        uint32 count = 0;
        for (ThreatReference const* ref : creature->GetThreatMgr().GetUnsortedThreatList())
        {
            if (!ref || !ref->IsAvailable())
                continue;

            Unit* victim = ref->GetVictim();
            if (!victim || !victim->IsPlayer())
                continue;

            Player* player = victim->ToPlayer();
            if (!player || !player->IsAlive() || player->IsGameMaster())
                continue;

            if (cfg.threatRange > 0.0f && !creature->IsWithinDist(player, cfg.threatRange))
                continue;

            ++count;
        }
        return count;
    }

    uint32 ClampPlayerCount(uint32 raw, uint8 designed)
    {
        uint32 floor = cfg.minPlayers;
        uint32 ceiling = designed ? designed : 40;
        if (floor > ceiling)
            floor = ceiling;
        return std::max(floor, std::min(raw ? raw : floor, ceiling));
    }

    float RatioFor(uint32 clamped, uint8 designed)
    {
        if (!designed)
            return 1.0f;
        return float(clamped) / float(designed);
    }

    void CacheOriginals(Creature* creature, WBSInfo* info)
    {
        if (!creature || !info || info->originalsCached)
            return;

        uint32 health = creature->GetCreateHealth();
        if (!health)
            health = creature->GetMaxHealth();
        info->originalCreateHealth = health ? health : 1;
        info->originalsCached = true;
    }

    void ApplyHealth(Creature* creature, WBSInfo* info, float newHealthMult)
    {
        if (!creature || !info || !info->originalsCached)
            return;

        uint32 const orig = info->originalCreateHealth ? info->originalCreateHealth : 1;
        uint32 const oldMax = creature->GetMaxHealth() ? creature->GetMaxHealth() : orig;
        float const pct = oldMax ? float(creature->GetHealth()) / float(oldMax) : 1.0f;

        uint32 newMax = uint32(std::round(float(orig) * newHealthMult));
        if (newMax < 1)
            newMax = 1;

        uint32 newCur = uint32(std::round(float(newMax) * pct));
        if (newCur < 1 && creature->IsAlive())
            newCur = 1;
        if (newCur > newMax)
            newCur = newMax;

        creature->SetCreateHealth(newMax);
        creature->SetMaxHealth(newMax);
        creature->SetHealth(newCur);
        creature->ResetPlayerDamageReq();
        info->healthMult = newHealthMult;
    }

    void RestoreOriginals(Creature* creature, WBSInfo* info)
    {
        if (!creature || !info || !info->originalsCached)
            return;

        uint32 const orig = info->originalCreateHealth ? info->originalCreateHealth : 1;
        creature->SetCreateHealth(orig);
        creature->SetMaxHealth(orig);
        if (creature->IsAlive())
            creature->SetHealth(orig);
        creature->ResetPlayerDamageReq();

        info->playerCount = 0;
        info->rawPlayerCount = 0;
        info->healthMult = 1.0f;
        info->damageMult = 1.0f;
        info->recountTimer = 0;
    }

    void ApplyScale(Creature* creature, WBSInfo* info, uint32 rawCount)
    {
        if (!creature || !info)
            return;

        uint8 designed = info->designedPlayers;
        if (!designed)
            designed = LookupDesigned(creature->GetEntry());
        if (!designed)
            return;

        info->designedPlayers = designed;
        info->rawPlayerCount = rawCount;
        uint32 const clamped = ClampPlayerCount(rawCount, designed);
        float const ratio = RatioFor(clamped, designed);
        float const newHealth = std::max(0.0f, ratio * cfg.healthMod);
        float const newDamage = std::max(0.0f, ratio * cfg.damageMod);

        CacheOriginals(creature, info);

        if (info->playerCount != clamped || std::fabs(info->healthMult - newHealth) > 0.0001f)
            ApplyHealth(creature, info, newHealth);

        info->playerCount = clamped;
        info->damageMult = newDamage;
    }

    void RecalcBoss(Creature* creature, WBSInfo* info)
    {
        ApplyScale(creature, info, CountThreatPlayers(creature));
    }

    void SyncSummon(Creature* creature, WBSInfo* info)
    {
        Creature* boss = GetSummonerCreature(creature);
        if (!boss)
            return;

        WBSInfo* bossInfo = GetInfo(boss, false);
        if (!bossInfo)
            return;

        info->designedPlayers = bossInfo->designedPlayers;
        ApplyScale(creature, info, bossInfo->rawPlayerCount ? bossInfo->rawPlayerCount : bossInfo->playerCount);
    }

    bool ShouldTrack(Creature* creature)
    {
        if (!cfg.enable || !creature || !creature->GetMap() || creature->GetMap()->IsDungeon())
            return false;
        return LookupDesigned(creature->GetEntry()) != 0;
    }

    bool TryTrackSummon(Creature* creature)
    {
        if (!cfg.enable || !creature || !creature->GetMap() || creature->GetMap()->IsDungeon())
            return false;

        Creature* boss = GetSummonerCreature(creature);
        if (!boss)
            return false;

        if (!LookupDesigned(boss->GetEntry()) && !GetInfo(boss, false))
            return false;

        WBSInfo* info = GetInfo(creature, true);
        info->isSummon = true;
        info->designedPlayers = LookupDesigned(boss->GetEntry());
        if (!info->designedPlayers)
            if (WBSInfo* bossInfo = GetInfo(boss, false))
                info->designedPlayers = bossInfo->designedPlayers;
        CacheOriginals(creature, info);
        SyncSummon(creature, info);
        return true;
    }

    uint32 ScaleOutgoing(Unit* attacker, uint32 amount)
    {
        WBSInfo* info = GetInfo(attacker);
        if (!info)
            return amount;
        return uint32(std::round(float(amount) * info->damageMult));
    }

    int32 ScaleOutgoing(Unit* attacker, int32 amount)
    {
        WBSInfo* info = GetInfo(attacker);
        if (!info)
            return amount;
        return int32(std::round(float(amount) * info->damageMult));
    }

    uint32 ScaleIncomingHeal(Unit* target, uint32 amount)
    {
        WBSInfo* info = GetInfo(target);
        if (!info)
            return amount;
        return uint32(std::round(float(amount) * info->healthMult));
    }
}

class WorldBossScale_World : public WorldScript
{
public:
    WorldBossScale_World() : WorldScript("WorldBossScale_World") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();

        if (!cfg.enable)
        {
            LOG_INFO("server.loading", "WorldBossScale: disabled");
            return;
        }

        if (cfg.announce)
        {
            LOG_INFO("server.loading", "WorldBossScale: {} catalog entries, min {}, healthMod {}, damageMod {}",
                DesignedPlayers.size(), cfg.minPlayers, cfg.healthMod, cfg.damageMod);
        }
        else
            LOG_INFO("server.loading", "WorldBossScale: module present ({} entries, min {})",
                DesignedPlayers.size(), cfg.minPlayers);
    }
};

class WorldBossScale_AllCreature : public AllCreatureScript
{
public:
    WorldBossScale_AllCreature() : AllCreatureScript("WorldBossScale_AllCreature") { }

    void OnCreatureAddWorld(Creature* creature) override
    {
        if (ShouldTrack(creature))
        {
            WBSInfo* info = GetInfo(creature, true);
            info->isSummon = false;
            info->designedPlayers = LookupDesigned(creature->GetEntry());
            CacheOriginals(creature, info);
            return;
        }

        TryTrackSummon(creature);
    }

    void OnAllCreatureUpdate(Creature* creature, uint32 diff) override
    {
        if (!cfg.enable || !creature)
            return;

        WBSInfo* info = GetInfo(creature, false);
        if (!info)
            return;

        if (info->isSummon)
        {
            info->recountTimer += diff;
            if (info->recountTimer < cfg.recountMs)
                return;
            info->recountTimer = 0;
            SyncSummon(creature, info);
            return;
        }

        if (!creature->IsInCombat())
            return;

        info->recountTimer += diff;
        if (info->recountTimer < cfg.recountMs)
            return;
        info->recountTimer = 0;
        RecalcBoss(creature, info);
    }
};

class WorldBossScale_Unit : public UnitScript
{
public:
    WorldBossScale_Unit() : UnitScript("WorldBossScale_Unit", true, {
        UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK,
        UNITHOOK_MODIFY_MELEE_DAMAGE,
        UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
        UNITHOOK_MODIFY_HEAL_RECEIVED,
        UNITHOOK_ON_UNIT_ENTER_COMBAT,
        UNITHOOK_ON_UNIT_ENTER_EVADE_MODE
    }) { }

    void OnUnitEnterCombat(Unit* unit, Unit* /*victim*/) override
    {
        if (!cfg.enable || !unit || !unit->IsCreature())
            return;

        Creature* creature = unit->ToCreature();
        WBSInfo* info = GetInfo(creature, false);
        if (!info || info->isSummon)
            return;

        RecalcBoss(creature, info);
    }

    void OnUnitEnterEvadeMode(Unit* unit, uint8 /*evadeReason*/) override
    {
        if (!unit || !unit->IsCreature())
            return;

        Creature* creature = unit->ToCreature();
        WBSInfo* info = GetInfo(creature, false);
        if (!info)
            return;

        RestoreOriginals(creature, info);
    }

    void ModifyPeriodicDamageAurasTick(Unit* /*target*/, Unit* attacker, uint32& damage, SpellInfo const* /*spellInfo*/) override
    {
        damage = ScaleOutgoing(attacker, damage);
    }

    void ModifyMeleeDamage(Unit* /*target*/, Unit* attacker, uint32& damage) override
    {
        damage = ScaleOutgoing(attacker, damage);
    }

    void ModifySpellDamageTaken(Unit* /*target*/, Unit* attacker, int32& damage, SpellInfo const* /*spellInfo*/) override
    {
        damage = ScaleOutgoing(attacker, damage);
    }

    void ModifyHealReceived(Unit* target, Unit* /*healer*/, uint32& heal, SpellInfo const* /*spellInfo*/) override
    {
        heal = ScaleIncomingHeal(target, heal);
    }
};

class WorldBossScale_Command : public CommandScript
{
public:
    WorldBossScale_Command() : CommandScript("WorldBossScale_Command") { }

    Acore::ChatCommands::ChatCommandTable GetCommands() const override
    {
        using namespace Acore::ChatCommands;

        static ChatCommandTable wbsTable =
        {
            { "status", HandleStatusCommand, SEC_GAMEMASTER, Console::No }
        };

        static ChatCommandTable commandTable =
        {
            { "wbs", wbsTable },
            { "worldbossscale", wbsTable }
        };

        return commandTable;
    }

    static bool HandleStatusCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        Unit* selected = player->GetSelectedUnit();
        Creature* creature = selected ? selected->ToCreature() : nullptr;
        if (!creature)
        {
            handler->PSendSysMessage("WorldBossScale: select a creature.");
            return true;
        }

        uint8 designed = LookupDesigned(creature->GetEntry());
        WBSInfo* info = GetInfo(creature, false);

        handler->PSendSysMessage("WorldBossScale: {} ({})", creature->GetName(), creature->GetEntry());
        handler->PSendSysMessage("  enabled: {}  catalog designed: {}",
            cfg.enable ? "yes" : "no", designed ? std::to_string(designed) : std::string("none"));

        if (!info)
        {
            handler->PSendSysMessage("  not tracked (not in catalog / not a boss summon).");
            return true;
        }

        handler->PSendSysMessage("  summon: {}  in combat: {}",
            info->isSummon ? "yes" : "no", creature->IsInCombat() ? "yes" : "no");
        handler->PSendSysMessage("  players: raw {}  clamped {} / designed {}",
            info->rawPlayerCount, info->playerCount, info->designedPlayers);
        handler->PSendSysMessage("  healthMult {:.4f}  damageMult {:.4f}", info->healthMult, info->damageMult);
        handler->PSendSysMessage("  HP original {}  create {}  current {}/{}",
            info->originalCreateHealth, creature->GetCreateHealth(), creature->GetHealth(), creature->GetMaxHealth());
        return true;
    }
};

void AddWorldBossScaleScripts()
{
    new WorldBossScale_World();
    new WorldBossScale_AllCreature();
    new WorldBossScale_Unit();
    new WorldBossScale_Command();
}
