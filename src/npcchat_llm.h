#ifndef MOD_NPCCHAT_LLM_H
#define MOD_NPCCHAT_LLM_H

#include <string>

// Minimal, self-contained API configuration for one LLM call.
// Kept as a plain struct (no globals) so this translation unit has zero
// coupling to the rest of the module — mirrors the spirit of PBC_APIConfig.
struct NpcChat_ApiConfig
{
    std::string baseUrl;       // e.g. https://openrouter.ai/api/v1
    std::string apiKey;        // Bearer token (empty = no auth header)
    std::string model;         // model identifier
    int         maxTokens = 150;
    double      temperature = 0.8;
    int         timeoutSec = 30;
    std::string extraParams;   // raw JSON fragment, single-quotes auto-converted
    bool        verifyCert = true;  // verify TLS cert on https endpoints (set false for self-signed/local proxy)
    int         maxConcurrent = 4;     // cap simultaneous in-flight LLM calls (0 = unlimited)
};

struct NpcChat_LLMResult
{
    bool        success = false;
    std::string text;          // assistant reply, trimmed, newlines collapsed
};

// Synchronous OpenAI-compatible /chat/completions call.
// Safe to call from any thread; touches no game objects.
NpcChat_LLMResult NpcChat_CallLLM(const NpcChat_ApiConfig& cfg,
    const std::string& systemPrompt,
    const std::string& userPrompt);

#endif // MOD_NPCCHAT_LLM_H
