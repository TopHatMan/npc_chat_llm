from pathlib import Path
import re


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly 1 anchor, found {count}")
    return text.replace(old, new, 1)


# -----------------------------------------------------------------------------
# Transport follow-up: fix capacity bookkeeping and add typed convenience calls.
# -----------------------------------------------------------------------------
transport = Path("src/npcchat_llm.cpp")
t = transport.read_text()

old_capacity = '''        if (_inFlightTotal >= allowed)\n        {\n            ++_failed;\n            ++_rejectedCapacity;\n            _lastError = NpcChat_LLMError::Capacity;\n            _lastHttpStatus = 0;\n            _lastFailureAt = std::time(nullptr);\n            _lastErrorDetail = interactive\n                ? "all interactive transport slots are busy"\n                : "background/generation capacity reserved for interactive chat";\n            _failureSinceLastSuccess = true;\n\n            rejection = MakeFailure(NpcChat_LLMError::Capacity, _lastErrorDetail);\n            return false;\n        }\n'''
new_capacity = '''        if (_inFlightTotal >= allowed)\n        {\n            ++_rejectedCapacity;\n            std::string const detail = interactive\n                ? "all interactive transport slots are busy"\n                : "background/generation capacity reserved for interactive chat";\n            rejection = MakeFailure(NpcChat_LLMError::Capacity, detail);\n            return false;\n        }\n'''
t = replace_once(t, old_capacity, new_capacity, "capacity bookkeeping")

append_wrappers = '''\nNpcChat_LLMResult NpcChat_CallBackgroundLLM(const NpcChat_ApiConfig& cfg,\n    const std::string& systemPrompt,\n    const std::string& userPrompt,\n    std::string_view label)\n{\n    return NpcChatTransport::Instance().Call(cfg, systemPrompt, userPrompt,\n        NpcChat_RequestClass::Background, label);\n}\n\nNpcChat_LLMResult NpcChat_CallGenerationLLM(const NpcChat_ApiConfig& cfg,\n    const std::string& systemPrompt,\n    const std::string& userPrompt,\n    std::string_view label)\n{\n    return NpcChatTransport::Instance().Call(cfg, systemPrompt, userPrompt,\n        NpcChat_RequestClass::Generation, label);\n}\n\nNpcChat_LLMResult NpcChat_CallHealthLLM(const NpcChat_ApiConfig& cfg,\n    const std::string& systemPrompt,\n    const std::string& userPrompt)\n{\n    return NpcChatTransport::Instance().Call(cfg, systemPrompt, userPrompt,\n        NpcChat_RequestClass::HealthCheck, "health-test");\n}\n'''
if "NpcChat_CallBackgroundLLM" not in t:
    t = t.rstrip() + "\n" + append_wrappers
transport.write_text(t)

header = Path("src/npcchat_llm.h")
h = header.read_text()
header_anchor = '''NpcChat_LLMResult NpcChat_CallLLM(const NpcChat_ApiConfig& cfg,\n    const std::string& systemPrompt,\n    const std::string& userPrompt,\n    NpcChat_RequestClass requestClass = NpcChat_RequestClass::Interactive,\n    std::string_view label = {});\n\nNpcChat_TransportSnapshot NpcChat_GetTransportSnapshot();\n'''
header_new = '''NpcChat_LLMResult NpcChat_CallLLM(const NpcChat_ApiConfig& cfg,\n    const std::string& systemPrompt,\n    const std::string& userPrompt,\n    NpcChat_RequestClass requestClass = NpcChat_RequestClass::Interactive,\n    std::string_view label = {});\n\nNpcChat_LLMResult NpcChat_CallBackgroundLLM(const NpcChat_ApiConfig& cfg,\n    const std::string& systemPrompt,\n    const std::string& userPrompt,\n    std::string_view label = "background");\n\nNpcChat_LLMResult NpcChat_CallGenerationLLM(const NpcChat_ApiConfig& cfg,\n    const std::string& systemPrompt,\n    const std::string& userPrompt,\n    std::string_view label = "generation");\n\nNpcChat_LLMResult NpcChat_CallHealthLLM(const NpcChat_ApiConfig& cfg,\n    const std::string& systemPrompt,\n    const std::string& userPrompt);\n\nNpcChat_TransportSnapshot NpcChat_GetTransportSnapshot();\n'''
h = replace_once(h, header_anchor, header_new, "transport wrapper declarations")
header.write_text(h)


# -----------------------------------------------------------------------------
# Main module stabilization wiring.
# -----------------------------------------------------------------------------
cpp = Path("src/mod_npcchat.cpp")
text = cpp.read_text()

text = replace_once(
    text,
    '''    int         g_MaxConcurrent = 4;     // NpcChat.Api.MaxConcurrentRequests (0 = unlimited)\n''',
    '''    int         g_MaxConcurrent = 4;     // NpcChat.Api.MaxConcurrentRequests (0 = unlimited)\n    int         g_ReserveInteractiveSlots = 1; // keep direct player conversations responsive\n    int         g_ApiErrorLogCooldownSec = 15; // rate-limit repeated identical transport errors\n    bool        g_NotifyInteractiveFailures = true; // visible failure instead of silent NPC chat\n''',
    "api config globals")

text = replace_once(
    text,
    '''        g_VerifyCert = sConfigMgr->GetOption<bool>("NpcChat.Api.VerifyCert", true);\n        g_MaxConcurrent = sConfigMgr->GetOption<int32>("NpcChat.Api.MaxConcurrentRequests", 4);\n''',
    '''        g_VerifyCert = sConfigMgr->GetOption<bool>("NpcChat.Api.VerifyCert", true);\n        g_MaxConcurrent = std::max(0, sConfigMgr->GetOption<int32>("NpcChat.Api.MaxConcurrentRequests", 4));\n        g_ReserveInteractiveSlots = std::max(0, sConfigMgr->GetOption<int32>("NpcChat.Api.ReserveInteractiveSlots", 1));\n        if (g_MaxConcurrent > 0)\n            g_ReserveInteractiveSlots = std::min(g_ReserveInteractiveSlots, g_MaxConcurrent);\n        g_ApiErrorLogCooldownSec = std::max(0, sConfigMgr->GetOption<int32>("NpcChat.Api.ErrorLogCooldownSec", 15));\n        g_NotifyInteractiveFailures = sConfigMgr->GetOption<bool>("NpcChat.Api.NotifyInteractiveFailures", true);\n''',
    "api config loading")

# Both live and generation config builders receive the same admission/diagnostic policy.
needle = '''        cfg.verifyCert = g_VerifyCert;\n        cfg.maxConcurrent = g_MaxConcurrent;\n        return cfg;\n'''
if text.count(needle) != 2:
    raise SystemExit(f"api config builders: expected 2 anchors, found {text.count(needle)}")
replacement = '''        cfg.verifyCert = g_VerifyCert;\n        cfg.maxConcurrent = g_MaxConcurrent;\n        cfg.reserveInteractiveSlots = g_ReserveInteractiveSlots;\n        cfg.errorLogCooldownSec = g_ApiErrorLogCooldownSec;\n        return cfg;\n'''
text = text.replace(needle, replacement)

# Direct NPC chat is the protected baseline and must never fail silently again.
direct_old = '''        NpcChat_LLMResult res = NpcChat_CallLLM(\n            cfg,\n            BuildSystemPrompt(req, defaultPrompt, sharedSubPrompts, sharedPrompt, personalSubPrompts, personalPrompt, relationshipText),\n            BuildUserPrompt(req, sharedHistory, personalHistory));\n\n        if (!res.success || res.text.empty())\n            return;\n'''
direct_new = '''        NpcChat_LLMResult res = NpcChat_CallLLM(\n            cfg,\n            BuildSystemPrompt(req, defaultPrompt, sharedSubPrompts, sharedPrompt, personalSubPrompts, personalPrompt, relationshipText),\n            BuildUserPrompt(req, sharedHistory, personalHistory),\n            NpcChat_RequestClass::Interactive,\n            "direct-npc");\n\n        if (!res.success || res.text.empty())\n        {\n            if (g_NotifyInteractiveFailures)\n                QueueSystemMessage(req.playerGuidRaw, "NPC Chat could not reply: " + NpcChat_FormatFailure(res) + "  Use .npcc health for transport status.");\n            return;\n        }\n'''
text = replace_once(text, direct_old, direct_new, "direct NPC failure visibility")

# Familiar-NPC recognition is automatic/background traffic.
history_old = '''        NpcChat_LLMResult res = NpcChat_CallLLM(\n            BuildChatApiConfig(),\n            BuildSystemPrompt(req, defaultPrompt, sharedSubPrompts, sharedPrompt, personalSubPrompts, personalPrompt, relationshipText),\n            user.str());\n'''
history_new = '''        NpcChat_LLMResult res = NpcChat_CallBackgroundLLM(\n            BuildChatApiConfig(),\n            BuildSystemPrompt(req, defaultPrompt, sharedSubPrompts, sharedPrompt, personalSubPrompts, personalPrompt, relationshipText),\n            user.str(),\n            "history-whisper");\n'''
text = replace_once(text, history_old, history_new, "history whisper priority")

# Multi-bot social chatter is ambient and must yield to direct chat.
social_old = '''            NpcChat_LLMResult res = NpcChat_CallLLM(BuildChatApiConfig(), system, user.str());\n'''
social_new = '''            NpcChat_LLMResult res = NpcChat_CallBackgroundLLM(BuildChatApiConfig(), system, user.str(), "bot-social");\n'''
text = replace_once(text, social_old, social_new, "bot social priority")

# Guild/LFG surface replies are useful but not allowed to starve core NPC interaction.
surface_pattern = re.compile(
    r'NpcChat_LLMResult res = NpcChat_CallLLM\(\s*BuildChatApiConfig\(\),\s*systemPrompt,\s*BuildBotUserPrompt\(promptReq, history\)\);')
text, surface_count = surface_pattern.subn(
    'NpcChat_LLMResult res = NpcChat_CallBackgroundLLM(\n            BuildChatApiConfig(), systemPrompt, BuildBotUserPrompt(promptReq, history), "bot-surface");',
    text,
    count=1)
if surface_count != 1:
    raise SystemExit(f"bot surface priority: expected 1 replacement, got {surface_count}")

# Any call using BuildGenerationApiConfig belongs to the lower-priority generation lane.
gen_pattern = re.compile(
    r'(NpcChat_ApiConfig cfg = BuildGenerationApiConfig\([^;]*\);(?:(?!NpcChat_ApiConfig cfg =).){0,1200}?NpcChat_LLMResult res = )NpcChat_CallLLM\(',
    re.S)
text, generation_count = gen_pattern.subn(r'\1NpcChat_CallGenerationLLM(', text)
if generation_count < 7:
    raise SystemExit(f"generation classification: expected at least 7 replacements, got {generation_count}")

# Command help gains a real transport-health diagnostic.
text = replace_once(
    text,
    '''            handler->PSendSysMessage(".npcc key");\n            handler->PSendSysMessage(".npcc status");\n            handler->PSendSysMessage(".npcc reload");\n''',
    '''            handler->PSendSysMessage(".npcc key");\n            handler->PSendSysMessage(".npcc status");\n            handler->PSendSysMessage(".npcc health [test|reset]");\n            handler->PSendSysMessage(".npcc reload");\n''',
    "command help")

status_anchor = '''        if (StartsWithWord(arg, "status", rest))\n        {\n'''
if text.count(status_anchor) != 1:
    raise SystemExit(f"status command anchor expected once, found {text.count(status_anchor)}")
health_block = r'''        if (StartsWithWord(arg, "health", rest))
        {
            std::string healthRest;
            if (StartsWithWord(rest, "reset", healthRest))
            {
                NpcChat_ResetTransportStats();
                handler->PSendSysMessage("NPC Chat transport counters reset. In-flight calls are not cancelled.");
                return true;
            }

            if (StartsWithWord(rest, "test", healthRest))
            {
                Player* player = GetCommandPlayer(handler);
                if (!player)
                {
                    handler->PSendSysMessage(".npcc health test must be run in game.");
                    return true;
                }

                NpcChat_ApiConfig cfg = BuildChatApiConfig();
                cfg.maxTokens = 8;
                cfg.temperature = 0.0;
                uint64_t const playerGuidRaw = player->GetGUID().GetRawValue();
                handler->PSendSysMessage("NPC Chat API health test queued; it runs off the world thread.");
                std::thread([playerGuidRaw, cfg]()
                    {
                        NpcChat_LLMResult res = NpcChat_CallHealthLLM(
                            cfg,
                            "You are a transport health check. Follow the user instruction exactly.",
                            "Reply with exactly: OK");
                        if (res.success)
                            QueueSystemMessage(playerGuidRaw, "NPC Chat API health test OK: provider/model returned a valid response.");
                        else
                            QueueSystemMessage(playerGuidRaw, "NPC Chat API health test FAILED: " + NpcChat_FormatFailure(res));
                    }).detach();
                return true;
            }

            NpcChat_TransportSnapshot const snapshot = NpcChat_GetTransportSnapshot();
            handler->PSendSysMessage("NPC Chat transport health:");
            handler->PSendSysMessage("  requests={} success={} failed={} capacityRejected={}",
                snapshot.totalRequests, snapshot.succeeded, snapshot.failed, snapshot.rejectedCapacity);
            handler->PSendSysMessage("  inFlight={} interactive={} background={} generation={}",
                snapshot.inFlightTotal, snapshot.inFlightInteractive, snapshot.inFlightBackground, snapshot.inFlightGeneration);
            handler->PSendSysMessage("  admission: maxConcurrent={} reservedInteractive={}",
                g_MaxConcurrent, g_ReserveInteractiveSlots);
            if (snapshot.lastError == NpcChat_LLMError::None)
                handler->PSendSysMessage("  last failure: (none recorded)");
            else
                handler->PSendSysMessage("  last failure: {} HTTP={} {}",
                    NpcChat_LLMErrorName(snapshot.lastError), snapshot.lastHttpStatus,
                    snapshot.lastErrorDetail.empty() ? "" : snapshot.lastErrorDetail.c_str());
            handler->PSendSysMessage("  active test: .npcc health test");
            return true;
        }

'''
text = text.replace(status_anchor, health_block + status_anchor, 1)

# Status also exposes admission settings so config + transport can be diagnosed together.
status_line = '''            handler->PSendSysMessage("  ApiKey: {}", g_ApiKey.empty() ? "missing" : "set");\n            handler->PSendSysMessage("  Config path: {}", sConfigMgr->GetConfigPath().c_str());\n'''
status_new = '''            handler->PSendSysMessage("  ApiKey: {}", g_ApiKey.empty() ? "missing" : "set");\n            handler->PSendSysMessage("  API concurrency: max={} reservedInteractive={} notifyInteractiveFailures={}",\n                g_MaxConcurrent, g_ReserveInteractiveSlots, g_NotifyInteractiveFailures ? "1" : "0");\n            handler->PSendSysMessage("  Config path: {}", sConfigMgr->GetConfigPath().c_str());\n'''
text = replace_once(text, status_line, status_new, "status transport config")

cpp.write_text(text)


# -----------------------------------------------------------------------------
# Canonical config: add the new transport policy once, next to MaxConcurrent.
# -----------------------------------------------------------------------------
conf = Path("conf/mod_npcchat.conf.dist")
c = conf.read_text()
c = replace_once(
    c,
    '''NpcChat.Api.VerifyCert = 1\nNpcChat.Api.MaxConcurrentRequests = 4\n''',
    '''NpcChat.Api.VerifyCert = 1\nNpcChat.Api.MaxConcurrentRequests = 4\n# Reserve capacity for direct player-driven NPC/bot chat. Background + generation calls yield first.\nNpcChat.Api.ReserveInteractiveSlots = 1\n# Repeated identical HTTP/provider failures are rate-limited in the worldserver log.\nNpcChat.Api.ErrorLogCooldownSec = 15\n# Direct targeted NPC chat reports a short in-game error instead of failing silently.\nNpcChat.Api.NotifyInteractiveFailures = 1\n''',
    "canonical transport config")
conf.write_text(c)


# README: document the protected baseline + health commands.
readme = Path("README.md")
r = readme.read_text()
section = r'''

## Stabilized transport and health checks

Direct targeted NPC conversation is the protected baseline. Automatic history whispers, social
playerbot chatter, guild/LFG surfaces, and generation jobs use lower-priority transport lanes so they
cannot consume every configured LLM slot. `NpcChat.Api.ReserveInteractiveSlots` controls the reserved
capacity (default `1` out of `4`).

The transport no longer turns connection/provider failures into unexplained silence. It tracks
request/success/failure/capacity counters, records the latest safe error, rate-limits repeated log
messages, and retries only failures that are plausibly transient (transport errors, HTTP 408/425/429,
and 5xx responses).

Runtime commands:

```text
.npcc status
.npcc health
.npcc health test
.npcc health reset
```

`.npcc health test` performs one tiny asynchronous model request and reports the result back in game;
it never blocks the world thread and never prints the API key or prompt contents to the server log.
'''
if "## Stabilized transport and health checks" not in r:
    r = r.rstrip() + section + "\n"
readme.write_text(r)

print(f"stabilization patch complete; generation calls classified: {generation_count}")
