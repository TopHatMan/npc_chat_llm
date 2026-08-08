#ifndef MOD_NPCCHAT_LLM_H
#define MOD_NPCCHAT_LLM_H

#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <string_view>

// Classify requests so background/automatic chatter cannot consume every API slot
// and starve direct player -> NPC conversations.
enum class NpcChat_RequestClass : std::uint8_t
{
    Interactive,
    Background,
    Generation,
    HealthCheck
};

enum class NpcChat_LLMError : std::uint8_t
{
    None,
    InvalidConfig,
    Capacity,
    InvalidUrl,
    TlsUnavailable,
    Transport,
    HttpStatus,
    Provider,
    MalformedResponse,
    EmptyResponse
};

// Self-contained API configuration for one LLM call. No game object pointers live here.
struct NpcChat_ApiConfig
{
    std::string baseUrl;       // e.g. https://openrouter.ai/api/v1
    std::string apiKey;        // Bearer token (empty = no auth header)
    std::string model;         // model identifier
    int         maxTokens = 150;
    double      temperature = 0.8;
    int         timeoutSec = 30;
    std::string extraParams;   // JSON object or fragment; legacy single-quote fragments are accepted
    bool        verifyCert = true;

    // Global transport admission control. Background/generation calls are capped below
    // maxConcurrent so direct interactive chat always has reserved capacity.
    int         maxConcurrent = 4;          // 0 = unlimited
    int         reserveInteractiveSlots = 1;
    int         errorLogCooldownSec = 15;   // rate-limit repeated identical transport warnings
};

struct NpcChat_LLMResult
{
    bool             success = false;
    std::string      text;          // assistant reply, trimmed, newlines collapsed
    NpcChat_LLMError error = NpcChat_LLMError::None;
    int              httpStatus = 0;
    int              attempts = 0;
    bool             retryable = false;
    std::string      detail;        // safe diagnostic detail; never contains the API key/request prompt
};

struct NpcChat_TransportSnapshot
{
    std::uint64_t totalRequests = 0;
    std::uint64_t succeeded = 0;
    std::uint64_t failed = 0;
    std::uint64_t rejectedCapacity = 0;

    int inFlightTotal = 0;
    int inFlightInteractive = 0;
    int inFlightBackground = 0;
    int inFlightGeneration = 0;

    NpcChat_LLMError lastError = NpcChat_LLMError::None;
    int lastHttpStatus = 0;
    std::time_t lastSuccessAt = 0;
    std::time_t lastFailureAt = 0;
    std::string lastErrorDetail;
};

// Thread-safe OpenAI-compatible transport. It owns admission control, retries,
// diagnostics, and error-rate limiting; it never touches AzerothCore game objects.
class NpcChatTransport
{
public:
    static NpcChatTransport& Instance();

    NpcChat_LLMResult Call(const NpcChat_ApiConfig& cfg,
        const std::string& systemPrompt,
        const std::string& userPrompt,
        NpcChat_RequestClass requestClass = NpcChat_RequestClass::Interactive,
        std::string_view label = {});

    NpcChat_TransportSnapshot GetSnapshot() const;
    void ResetStats();

private:
    NpcChatTransport() = default;
    NpcChatTransport(NpcChatTransport const&) = delete;
    NpcChatTransport& operator=(NpcChatTransport const&) = delete;

    bool TryAcquire(const NpcChat_ApiConfig& cfg, NpcChat_RequestClass requestClass,
        NpcChat_LLMResult& rejection);
    void Release(NpcChat_RequestClass requestClass);
    void RecordSuccess(NpcChat_RequestClass requestClass);
    void RecordFailure(const NpcChat_ApiConfig& cfg, NpcChat_RequestClass requestClass,
        std::string_view label, NpcChat_LLMResult const& result);

    mutable std::mutex _stateMutex;
    std::uint64_t _totalRequests = 0;
    std::uint64_t _succeeded = 0;
    std::uint64_t _failed = 0;
    std::uint64_t _rejectedCapacity = 0;

    int _inFlightTotal = 0;
    int _inFlightInteractive = 0;
    int _inFlightBackground = 0;
    int _inFlightGeneration = 0;

    NpcChat_LLMError _lastError = NpcChat_LLMError::None;
    int _lastHttpStatus = 0;
    std::time_t _lastSuccessAt = 0;
    std::time_t _lastFailureAt = 0;
    std::string _lastErrorDetail;

    std::time_t _nextSameErrorLogAt = 0;
    std::string _lastLoggedErrorKey;
    bool _failureSinceLastSuccess = false;
};

// Compatibility wrapper used by the rest of the module.
NpcChat_LLMResult NpcChat_CallLLM(const NpcChat_ApiConfig& cfg,
    const std::string& systemPrompt,
    const std::string& userPrompt,
    NpcChat_RequestClass requestClass = NpcChat_RequestClass::Interactive,
    std::string_view label = {});

NpcChat_TransportSnapshot NpcChat_GetTransportSnapshot();
void NpcChat_ResetTransportStats();

const char* NpcChat_RequestClassName(NpcChat_RequestClass requestClass);
const char* NpcChat_LLMErrorName(NpcChat_LLMError error);
std::string NpcChat_FormatFailure(NpcChat_LLMResult const& result);

#endif // MOD_NPCCHAT_LLM_H
