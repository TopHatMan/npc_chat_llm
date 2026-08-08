from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one anchor, found {count}")
    return text.replace(old, new, 1)


p = Path("src/mod_npcchat.cpp")
s = p.read_text()

# Quieter automatic recognition whispers. These defaults mirror the canonical config.
s = replace_once(
    s,
    '''    int         g_HistoryWhispersChancePct = 12;\n    int         g_HistoryWhispersPlayerCooldownSec = 300;\n    int         g_HistoryWhispersPairCooldownSec = 900;\n    int         g_HistoryWhispersScanIntervalMs = 5000;\n''',
    '''    int         g_HistoryWhispersChancePct = 5;\n    int         g_HistoryWhispersPlayerCooldownSec = 600;\n    int         g_HistoryWhispersPairCooldownSec = 1800;\n    int         g_HistoryWhispersScanIntervalMs = 8000;\n''',
    "history whisper globals")

s = replace_once(
    s,
    '''        g_HistoryWhispersChancePct = sConfigMgr->GetOption<int32>("NpcChat.HistoryWhispers.ChancePct", 12);\n        g_HistoryWhispersPlayerCooldownSec = sConfigMgr->GetOption<int32>("NpcChat.HistoryWhispers.PlayerCooldownSec", 300);\n        g_HistoryWhispersPairCooldownSec = sConfigMgr->GetOption<int32>("NpcChat.HistoryWhispers.PairCooldownSec", 900);\n        g_HistoryWhispersScanIntervalMs = sConfigMgr->GetOption<int32>("NpcChat.HistoryWhispers.ScanIntervalMs", 5000);\n''',
    '''        g_HistoryWhispersChancePct = sConfigMgr->GetOption<int32>("NpcChat.HistoryWhispers.ChancePct", 5);\n        g_HistoryWhispersPlayerCooldownSec = sConfigMgr->GetOption<int32>("NpcChat.HistoryWhispers.PlayerCooldownSec", 600);\n        g_HistoryWhispersPairCooldownSec = sConfigMgr->GetOption<int32>("NpcChat.HistoryWhispers.PairCooldownSec", 1800);\n        g_HistoryWhispersScanIntervalMs = sConfigMgr->GetOption<int32>("NpcChat.HistoryWhispers.ScanIntervalMs", 8000);\n''',
    "history whisper config defaults")

# Fix all-random guild/raid pools: previously one random bot was selected and the final
# fill loop required the output to still be empty, so MaxSpeakers could never be reached.
s = replace_once(
    s,
    '''        std::vector<Player*> out;\n        auto addUnique = [&](Player* bot)\n''',
    '''        bool const randomOnlyPool = named.empty() && alts.empty() && !randoms.empty();\n\n        std::vector<Player*> out;\n        auto addUnique = [&](Player* bot)\n''',
    "random-only social pool marker")

s = replace_once(
    s,
    '''        while (static_cast<int>(out.size()) < maxSpeakers && !alts.empty())\n            addUnique(TakeRandomBot(alts));\n        while (static_cast<int>(out.size()) < maxSpeakers && out.empty() && !randoms.empty())\n            addUnique(TakeRandomBot(randoms));\n\n        return out;\n''',
    '''        while (static_cast<int>(out.size()) < maxSpeakers && !alts.empty())\n            addUnique(TakeRandomBot(alts));\n\n        // If the entire eligible pool is random Playerbots, it is still valid to use the\n        // configured speaker cap. The old out.empty() condition limited these groups to one voice.\n        while (static_cast<int>(out.size()) < maxSpeakers && randomOnlyPool && !randoms.empty())\n            addUnique(TakeRandomBot(randoms));\n\n        return out;\n''',
    "random-only social fill")

# Autonomous guild chatter is intentionally separate from player-provoked social chat. It uses
# a small per-guild transcript and direct guild broadcast, so it cannot recursively invoke hooks.
ambient_code = r'''
    // --- autonomous guild ambience ----------------------------------------------
    struct GuildAmbientCfg
    {
        bool enable = true;
        uint32 minIntervalSec = 90;
        uint32 maxIntervalSec = 240;
        int maxSpeakers = 2;
        uint32 randomBotChancePct = 15;
        int historyTail = 12;
    };

    inline GuildAmbientCfg GetGuildAmbientCfg()
    {
        GuildAmbientCfg c;
        c.enable = sConfigMgr->GetOption<bool>("NpcChat.Bot.GuildAmbient.Enable", true, false);
        c.minIntervalSec = std::max<uint32>(15,
            sConfigMgr->GetOption<uint32>("NpcChat.Bot.GuildAmbient.MinIntervalSec", 90, false));
        c.maxIntervalSec = std::max<uint32>(c.minIntervalSec,
            sConfigMgr->GetOption<uint32>("NpcChat.Bot.GuildAmbient.MaxIntervalSec", 240, false));
        c.maxSpeakers = std::max(1, std::min(3,
            sConfigMgr->GetOption<int32>("NpcChat.Bot.GuildAmbient.MaxSpeakers", 2, false)));
        c.randomBotChancePct = std::min<uint32>(100,
            sConfigMgr->GetOption<uint32>("NpcChat.Bot.GuildAmbient.RandomBotChancePct", 15, false));
        c.historyTail = std::max(0, std::min(30,
            sConfigMgr->GetOption<int32>("NpcChat.Bot.GuildAmbient.HistoryMaxLines", 12, false)));
        return c;
    }

    inline std::map<uint32, time_t>& GuildAmbientNextAt()
    {
        static std::map<uint32, time_t> nextAt;
        return nextAt;
    }

    inline uint32 GuildAmbientDelay(GuildAmbientCfg const& cfg)
    {
        if (cfg.maxIntervalSec <= cfg.minIntervalSec)
            return cfg.minIntervalSec;
        uint32 const span = cfg.maxIntervalSec - cfg.minIntervalSec + 1;
        return cfg.minIntervalSec + static_cast<uint32>(std::rand()) % span;
    }

    inline std::string BotGuildAmbientHistoryFilePath(uint32 guildId)
    {
        return g_HistoryPath + "/bots/guild/" + std::to_string(guildId) + ".history";
    }

    struct BotGuildAmbientConversationRequest
    {
        uint32 guildId = 0;
        int historyTail = 12;
        std::vector<BotChatRequest> turns;
    };

    inline void BotGuildAmbientConversationWorker(BotGuildAmbientConversationRequest req)
    {
        std::string const historyPath = BotGuildAmbientHistoryFilePath(req.guildId);
        std::deque<std::string> recentGuildHistory;
        {
            std::lock_guard<std::mutex> lock(g_FileMutex);
            try { std::filesystem::create_directories(g_HistoryPath + "/bots/guild"); }
            catch (std::exception const&) {}
            recentGuildHistory = LoadHistoryTail(historyPath, req.historyTail);
        }

        std::string conversation;
        for (size_t i = 0; i < req.turns.size(); ++i)
        {
            BotChatRequest turn = req.turns[i];
            std::string card;
            {
                std::lock_guard<std::mutex> lock(g_FileMutex);
                EnsureBotChatDirectories();
                card = LoadBotCharacterCard(turn.botName);
            }
            card = ExpandBotCharacterCard(turn, std::move(card));

            std::string system = BuildBotSystemPrompt(turn, card);
            system += "\n\nYou are casually participating in guild chat without being directly addressed by a player. "
                "Sound like an adventurer who actually belongs to this guild. Keep it short and ordinary. "
                "Good topics include where you are headed, questing, dungeons, professions, supplies, gear, "
                "travel, asking what guildmates are doing, or a light joke. Do not invent major server events, "
                "boss kills, rare loot drops, emergencies, or facts you were not given.";

            std::ostringstream user;
            if (!recentGuildHistory.empty())
            {
                user << "Recent ambient guild chat:\n";
                for (std::string const& line : recentGuildHistory)
                    user << line << "\n";
                user << "\n";
            }

            if (conversation.empty())
            {
                user << "Start one short, natural guild-chat line as " << turn.botName
                    << ". This is an idle social moment, not a response to a hidden player message. "
                    << "If nothing suitable comes to mind, output exactly [SKIP].";
            }
            else
            {
                user << "Guild conversation so far:\n" << conversation
                    << "\n\nAdd one short natural guild-chat reply as " << turn.botName
                    << ". React only to what was actually said. If you truly have nothing to add, output exactly [SKIP].";
            }

            NpcChat_LLMResult res = NpcChat_CallBackgroundLLM(
                BuildChatApiConfig(), system, user.str(), "guild-ambient");
            std::string line = TrimCopy(res.text);
            if (!res.success || line.empty() || ToLowerCopy(line) == "[skip]")
                continue;

            {
                std::lock_guard<std::mutex> lock(g_FileMutex);
                AppendHistoryLine(historyPath, turn.botName + ": " + line);
            }
            recentGuildHistory.push_back(turn.botName + ": " + line);
            while (static_cast<int>(recentGuildHistory.size()) > req.historyTail && !recentGuildHistory.empty())
                recentGuildHistory.pop_front();

            if (!conversation.empty())
                conversation += "\n";
            conversation += turn.botName + ": " + line;

            BotSocialReply reply;
            reply.botGuidRaw = turn.botGuidRaw;
            reply.channel = BotSocialChannel::Guild;
            reply.text = std::move(line);
            std::lock_guard<std::mutex> lock(BotSocialReplyMutex());
            BotSocialReplyQueue().push(std::move(reply));
        }
    }

    inline void MaybeStartGuildAmbientChat(Player* realPlayer)
    {
        if (!IsRealPlayerSession(realPlayer))
            return;

        GuildAmbientCfg const cfg = GetGuildAmbientCfg();
        if (!cfg.enable)
            return;

        uint32 const guildId = realPlayer->GetGuildId();
        Guild* guild = realPlayer->GetGuild();
        if (!guildId || !guild)
            return;

        time_t const now = std::time(nullptr);
        time_t& nextAt = GuildAmbientNextAt()[guildId];
        if (nextAt == 0)
        {
            nextAt = now + GuildAmbientDelay(cfg);
            return; // never chatter immediately just because a player logged in
        }
        if (now < nextAt)
            return;

        // Schedule the next attempt before doing any work so multiple real members updating on
        // the same world tick cannot multiply the ambient rate for one guild.
        nextAt = now + GuildAmbientDelay(cfg);

        std::vector<Player*> candidates;
        auto collectOnlineBot = [&](Player* member)
        {
            if (member && member != realPlayer && IsGenuineBot(member) && member->GetGuildId() == guildId)
                candidates.push_back(member);
        };
        guild->BroadcastWorker(collectOnlineBot, realPlayer);
        if (candidates.empty())
            return;

        std::vector<Player*> chosen = ChooseSocialBots(
            candidates, "", cfg.maxSpeakers, cfg.randomBotChancePct);
        if (chosen.empty())
            return;

        BotGuildAmbientConversationRequest req;
        req.guildId = guildId;
        req.historyTail = cfg.historyTail;
        for (Player* bot : chosen)
        {
            if (!bot || !IsGenuineBot(bot))
                continue;
            BotChatRequest turn;
            turn.botGuidRaw = bot->GetGUID().GetRawValue();
            turn.botName = bot->GetName();
            turn.botLevel = bot->GetLevel();
            turn.botGender = BotGenderName(bot->getGender());
            turn.botRace = BotRaceName(bot->getRace());
            turn.botClass = BotClassName(bot->getClass());
            req.turns.push_back(std::move(turn));
        }
        if (!req.turns.empty())
            std::thread(BotGuildAmbientConversationWorker, std::move(req)).detach();
    }

'''

anchor = '''    inline void DispatchBotSocialConversation(Player* player, std::vector<Player*> const& bots,\n'''
if s.count(anchor) != 1:
    raise SystemExit(f"ambient insertion anchor: expected exactly one, found {s.count(anchor)}")
s = s.replace(anchor, ambient_code + anchor, 1)

# Let the world-thread player update drive the per-guild timer. MaybeStart... is itself guild-keyed,
# so multiple real members do not create multiple ambient clocks.
s = replace_once(
    s,
    '''        EnsureGuildBotsOnline(player);\n\n        if (!player->IsAlive())\n''',
    '''        EnsureGuildBotsOnline(player);\n        MaybeStartGuildAmbientChat(player);\n\n        if (!player->IsAlive())\n''',
    "ambient player update hook")

p.write_text(s)
print("guild ambient tuning patch applied")
