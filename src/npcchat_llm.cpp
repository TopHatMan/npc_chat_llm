#ifdef _WIN32
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "npcchat_llm.h"
#include "Log.h"

// Rename the httplib namespace so this module can be loaded alongside
// mod-playerbots-characters (which does the same with pbc_httplib) without
// an ODR violation.
#define httplib npcchat_httplib
#include <httplib.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <regex>
#include <sstream>
#include <thread>
#include <utility>

using json = nlohmann::json;

namespace
{
    struct HttpResponse
    {
        bool ok = false;
        int status = 0;
        NpcChat_LLMError error = NpcChat_LLMError::None;
        bool retryable = false;
        std::string body;
        std::string detail;
    };

    std::string Trim(std::string s)
    {
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        return s;
    }

    std::string OneLine(std::string text, std::size_t maxLen = 280)
    {
        for (char& c : text)
        {
            if (c == '\n' || c == '\r' || c == '\t')
                c = ' ';
        }
        text = Trim(std::move(text));
        if (text.size() > maxLen)
        {
            text.resize(maxLen);
            text += "...";
        }
        return text;
    }

    bool IsRetryableStatus(int status)
    {
        return status == 408 || status == 425 || status == 429 || status >= 500;
    }

    std::string ProviderErrorMessage(std::string const& body)
    {
        if (body.empty())
            return {};

        try
        {
            json j = json::parse(body);
            if (j.contains("error"))
            {
                json const& error = j["error"];
                if (error.is_string())
                    return OneLine(error.get<std::string>());
                if (error.is_object())
                {
                    if (error.contains("message") && error["message"].is_string())
                        return OneLine(error["message"].get<std::string>());
                    return OneLine(error.dump());
                }
            }
            if (j.contains("message") && j["message"].is_string())
                return OneLine(j["message"].get<std::string>());
        }
        catch (std::exception const&)
        {
            // Non-JSON error bodies are still useful, but keep them short and one-line.
        }

        return OneLine(body);
    }

    bool BuildEndpoint(std::string baseUrl, std::string& endpoint, std::string& error)
    {
        baseUrl = Trim(std::move(baseUrl));
        if (baseUrl.empty())
        {
            error = "BaseUrl is empty";
            return false;
        }

        while (baseUrl.size() > 1 && baseUrl.back() == '/')
            baseUrl.pop_back();

        static const std::string suffix = "/chat/completions";
        if (baseUrl.size() >= suffix.size() &&
            baseUrl.compare(baseUrl.size() - suffix.size(), suffix.size(), suffix) == 0)
            endpoint = std::move(baseUrl);
        else
            endpoint = std::move(baseUrl) + suffix;

        return true;
    }

    bool MergeExtraParameters(json& body, std::string const& raw, std::string& error)
    {
        if (Trim(raw).empty())
            return true;

        auto tryParse = [](std::string text, json& out) -> bool
        {
            text = Trim(std::move(text));
            if (text.empty())
                return true;
            try
            {
                if (text.front() != '{')
                    text = "{" + text + "}";
                out = json::parse(text);
                return out.is_object();
            }
            catch (std::exception const&)
            {
                return false;
            }
        };

        json extra;
        if (!tryParse(raw, extra))
        {
            // Backward compatibility for the old documented single-quote fragment style.
            std::string legacy = raw;
            std::replace(legacy.begin(), legacy.end(), '\'', '"');
            if (!tryParse(legacy, extra))
            {
                error = "ModelExtraParameters is not valid JSON";
                return false;
            }
        }

        for (auto it = extra.begin(); it != extra.end(); ++it)
            body[it.key()] = it.value();
        return true;
    }

    HttpResponse HttpPost(std::string const& url,
        std::string const& body,
        std::string const& apiKey,
        int timeoutSec,
        bool verifyCert)
    {
        HttpResponse out;
        try
        {
            static const std::regex urlRe(R"(^(https?)://([^:/]+)(?::(\d+))?(/.*)?$)");
            std::smatch m;
            if (!std::regex_match(url, m, urlRe))
            {
                out.error = NpcChat_LLMError::InvalidUrl;
                out.detail = "invalid URL: expected http(s)://host[:port]/path";
                return out;
            }

            std::string proto = m[1].str();
            std::string host = m[2].str();
            std::string path = m[4].matched ? m[4].str() : "/";
            int port = proto == "https" ? 443 : 80;
            if (m[3].matched)
                port = std::stoi(m[3].str());

            httplib::Headers headers = {
                {"Accept", "application/json"},
                {"User-Agent", "AzerothCore-NpcChat/2.0"},
            };
            if (!apiKey.empty())
                headers.emplace("Authorization", "Bearer " + apiKey);

            httplib::Result res;
            int const timeout = std::max(1, timeoutSec);
            if (proto == "https")
            {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
                httplib::SSLClient cli(host, port);
                cli.enable_server_certificate_verification(verifyCert);
                cli.set_connection_timeout(timeout);
                cli.set_read_timeout(timeout);
                cli.set_write_timeout(timeout);
                res = cli.Post(path, headers, body, "application/json");
#else
                out.error = NpcChat_LLMError::TlsUnavailable;
                out.detail = "HTTPS requested but this build has no cpp-httplib OpenSSL support";
                return out;
#endif
            }
            else
            {
                httplib::Client cli(host, port);
                cli.set_connection_timeout(timeout);
                cli.set_read_timeout(timeout);
                cli.set_write_timeout(timeout);
                res = cli.Post(path, headers, body, "application/json");
            }

            if (!res)
            {
                out.error = NpcChat_LLMError::Transport;
                out.retryable = true;
                out.detail = "HTTP transport failed before a response was received";
                return out;
            }

            out.status = res->status;
            out.body = res->body;
            if (res->status < 200 || res->status >= 300)
            {
                out.error = NpcChat_LLMError::HttpStatus;
                out.retryable = IsRetryableStatus(res->status);
                std::string provider = ProviderErrorMessage(res->body);
                out.detail = "HTTP " + std::to_string(res->status);
                if (!provider.empty())
                    out.detail += ": " + provider;
                return out;
            }

            out.ok = true;
            return out;
        }
        catch (std::exception const& e)
        {
            out.error = NpcChat_LLMError::Transport;
            out.retryable = true;
            out.detail = "transport exception: " + OneLine(e.what());
            return out;
        }
    }

    NpcChat_LLMResult MakeFailure(NpcChat_LLMError error, std::string detail,
        int httpStatus = 0, bool retryable = false, int attempts = 0)
    {
        NpcChat_LLMResult result;
        result.error = error;
        result.detail = OneLine(std::move(detail));
        result.httpStatus = httpStatus;
        result.retryable = retryable;
        result.attempts = attempts;
        return result;
    }
}

const char* NpcChat_RequestClassName(NpcChat_RequestClass requestClass)
{
    switch (requestClass)
    {
        case NpcChat_RequestClass::Interactive: return "interactive";
        case NpcChat_RequestClass::Background:  return "background";
        case NpcChat_RequestClass::Generation:  return "generation";
        case NpcChat_RequestClass::HealthCheck: return "health";
    }
    return "unknown";
}

const char* NpcChat_LLMErrorName(NpcChat_LLMError error)
{
    switch (error)
    {
        case NpcChat_LLMError::None:              return "none";
        case NpcChat_LLMError::InvalidConfig:     return "invalid_config";
        case NpcChat_LLMError::Capacity:          return "capacity";
        case NpcChat_LLMError::InvalidUrl:        return "invalid_url";
        case NpcChat_LLMError::TlsUnavailable:    return "tls_unavailable";
        case NpcChat_LLMError::Transport:         return "transport";
        case NpcChat_LLMError::HttpStatus:        return "http_status";
        case NpcChat_LLMError::Provider:          return "provider";
        case NpcChat_LLMError::MalformedResponse: return "malformed_response";
        case NpcChat_LLMError::EmptyResponse:     return "empty_response";
    }
    return "unknown";
}

std::string NpcChat_FormatFailure(NpcChat_LLMResult const& result)
{
    if (result.success)
        return "ok";

    std::ostringstream ss;
    ss << NpcChat_LLMErrorName(result.error);
    if (result.httpStatus > 0)
        ss << " (HTTP " << result.httpStatus << ")";
    if (!result.detail.empty())
        ss << ": " << result.detail;
    if (result.attempts > 0)
        ss << " [attempts=" << result.attempts << "]";
    return ss.str();
}

NpcChatTransport& NpcChatTransport::Instance()
{
    static NpcChatTransport instance;
    return instance;
}

bool NpcChatTransport::TryAcquire(const NpcChat_ApiConfig& cfg,
    NpcChat_RequestClass requestClass, NpcChat_LLMResult& rejection)
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    ++_totalRequests;

    if (cfg.maxConcurrent <= 0)
    {
        ++_inFlightTotal;
    }
    else
    {
        int const maxConcurrent = std::max(1, cfg.maxConcurrent);
        int const reserve = std::clamp(cfg.reserveInteractiveSlots, 0, maxConcurrent);
        bool const interactive = requestClass == NpcChat_RequestClass::Interactive ||
            requestClass == NpcChat_RequestClass::HealthCheck;
        int const allowed = interactive ? maxConcurrent : std::max(0, maxConcurrent - reserve);

        if (_inFlightTotal >= allowed)
        {
            ++_rejectedCapacity;
            std::string const detail = interactive
                ? "all interactive transport slots are busy"
                : "background/generation capacity reserved for interactive chat";
            rejection = MakeFailure(NpcChat_LLMError::Capacity, detail);
            return false;
        }

        ++_inFlightTotal;
    }

    switch (requestClass)
    {
        case NpcChat_RequestClass::Interactive:
        case NpcChat_RequestClass::HealthCheck:
            ++_inFlightInteractive;
            break;
        case NpcChat_RequestClass::Background:
            ++_inFlightBackground;
            break;
        case NpcChat_RequestClass::Generation:
            ++_inFlightGeneration;
            break;
    }
    return true;
}

void NpcChatTransport::Release(NpcChat_RequestClass requestClass)
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    _inFlightTotal = std::max(0, _inFlightTotal - 1);
    switch (requestClass)
    {
        case NpcChat_RequestClass::Interactive:
        case NpcChat_RequestClass::HealthCheck:
            _inFlightInteractive = std::max(0, _inFlightInteractive - 1);
            break;
        case NpcChat_RequestClass::Background:
            _inFlightBackground = std::max(0, _inFlightBackground - 1);
            break;
        case NpcChat_RequestClass::Generation:
            _inFlightGeneration = std::max(0, _inFlightGeneration - 1);
            break;
    }
}

void NpcChatTransport::RecordSuccess(NpcChat_RequestClass requestClass)
{
    bool recovered = false;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        ++_succeeded;
        _lastSuccessAt = std::time(nullptr);
        recovered = _failureSinceLastSuccess;
        _failureSinceLastSuccess = false;
    }

    if (recovered)
        LOG_INFO("module", "[NpcChat] LLM transport recovered on a {} request.", NpcChat_RequestClassName(requestClass));
}

void NpcChatTransport::RecordFailure(const NpcChat_ApiConfig& cfg,
    NpcChat_RequestClass requestClass, std::string_view label, NpcChat_LLMResult const& result)
{
    bool shouldLog = false;
    std::string logKey;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        ++_failed;
        _lastError = result.error;
        _lastHttpStatus = result.httpStatus;
        _lastFailureAt = std::time(nullptr);
        _lastErrorDetail = result.detail;
        _failureSinceLastSuccess = true;

        logKey = std::string(NpcChat_LLMErrorName(result.error)) + "|" +
            std::to_string(result.httpStatus) + "|" + result.detail;
        std::time_t const now = std::time(nullptr);
        int const cooldown = std::max(0, cfg.errorLogCooldownSec);
        if (logKey != _lastLoggedErrorKey || cooldown == 0 || now >= _nextSameErrorLogAt)
        {
            shouldLog = true;
            _lastLoggedErrorKey = logKey;
            _nextSameErrorLogAt = now + cooldown;
        }
    }

    if (shouldLog)
    {
        std::string labelText;
        if (!label.empty())
            labelText = " [" + std::string(label) + "]";
        LOG_WARN("module", "[NpcChat] LLM {} request{} failed: {}",
            NpcChat_RequestClassName(requestClass), labelText, NpcChat_FormatFailure(result));
    }
}

NpcChat_LLMResult NpcChatTransport::Call(const NpcChat_ApiConfig& cfg,
    const std::string& systemPrompt,
    const std::string& userPrompt,
    NpcChat_RequestClass requestClass,
    std::string_view label)
{
    if (Trim(cfg.baseUrl).empty())
    {
        NpcChat_LLMResult result = MakeFailure(NpcChat_LLMError::InvalidConfig, "NpcChat.BaseUrl is empty");
        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            ++_totalRequests;
        }
        RecordFailure(cfg, requestClass, label, result);
        return result;
    }
    if (Trim(cfg.model).empty())
    {
        NpcChat_LLMResult result = MakeFailure(NpcChat_LLMError::InvalidConfig, "NpcChat.Model is empty");
        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            ++_totalRequests;
        }
        RecordFailure(cfg, requestClass, label, result);
        return result;
    }

    std::string endpoint;
    std::string endpointError;
    if (!BuildEndpoint(cfg.baseUrl, endpoint, endpointError))
    {
        NpcChat_LLMResult result = MakeFailure(NpcChat_LLMError::InvalidUrl, endpointError);
        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            ++_totalRequests;
        }
        RecordFailure(cfg, requestClass, label, result);
        return result;
    }

    json body;
    body["model"] = cfg.model;
    body["temperature"] = cfg.temperature;
    if (cfg.maxTokens > 0)
        body["max_tokens"] = cfg.maxTokens;

    json messages = json::array();
    if (!systemPrompt.empty())
        messages.push_back({ {"role", "system"}, {"content", systemPrompt} });
    messages.push_back({ {"role", "user"}, {"content", userPrompt} });
    body["messages"] = messages;

    std::string extraError;
    if (!MergeExtraParameters(body, cfg.extraParams, extraError))
    {
        NpcChat_LLMResult result = MakeFailure(NpcChat_LLMError::InvalidConfig, extraError);
        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            ++_totalRequests;
        }
        RecordFailure(cfg, requestClass, label, result);
        return result;
    }

    NpcChat_LLMResult rejection;
    if (!TryAcquire(cfg, requestClass, rejection))
    {
        RecordFailure(cfg, requestClass, label, rejection);
        return rejection;
    }

    struct SlotGuard
    {
        NpcChatTransport& transport;
        NpcChat_RequestClass requestClass;
        ~SlotGuard() { transport.Release(requestClass); }
    } guard{ *this, requestClass };

    std::string const bodyStr = body.dump();
    constexpr int MAX_ATTEMPTS = 2;
    NpcChat_LLMResult lastFailure;

    for (int attempt = 1; attempt <= MAX_ATTEMPTS; ++attempt)
    {
        if (attempt > 1)
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        HttpResponse http = HttpPost(endpoint, bodyStr, cfg.apiKey, cfg.timeoutSec, cfg.verifyCert);
        if (!http.ok)
        {
            lastFailure = MakeFailure(http.error, http.detail, http.status, http.retryable, attempt);
            if (!http.retryable || attempt == MAX_ATTEMPTS)
                break;
            continue;
        }

        if (http.body.empty())
        {
            lastFailure = MakeFailure(NpcChat_LLMError::EmptyResponse,
                "provider returned an empty HTTP response body", http.status, true, attempt);
            if (attempt == MAX_ATTEMPTS)
                break;
            continue;
        }

        try
        {
            json j = json::parse(http.body);
            if (j.contains("error"))
            {
                std::string provider = ProviderErrorMessage(http.body);
                lastFailure = MakeFailure(NpcChat_LLMError::Provider,
                    provider.empty() ? "provider returned an error object" : provider,
                    http.status, false, attempt);
                break;
            }

            if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty() ||
                !j["choices"][0].contains("message") ||
                !j["choices"][0]["message"].contains("content") ||
                !j["choices"][0]["message"]["content"].is_string())
            {
                lastFailure = MakeFailure(NpcChat_LLMError::MalformedResponse,
                    "response did not contain choices[0].message.content",
                    http.status, false, attempt);
                break;
            }

            std::string text = Trim(j["choices"][0]["message"]["content"].get<std::string>());
            if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                text = text.substr(1, text.size() - 2);
            for (char& c : text)
            {
                if (c == '\n' || c == '\r')
                    c = ' ';
            }
            text = Trim(std::move(text));

            if (text.empty())
            {
                lastFailure = MakeFailure(NpcChat_LLMError::EmptyResponse,
                    "provider returned an empty assistant message", http.status, true, attempt);
                if (attempt == MAX_ATTEMPTS)
                    break;
                continue;
            }

            NpcChat_LLMResult result;
            result.success = true;
            result.text = std::move(text);
            result.httpStatus = http.status;
            result.attempts = attempt;
            RecordSuccess(requestClass);
            return result;
        }
        catch (std::exception const& e)
        {
            lastFailure = MakeFailure(NpcChat_LLMError::MalformedResponse,
                "JSON parse/read failed: " + OneLine(e.what()), http.status, false, attempt);
            break;
        }
    }

    if (lastFailure.error == NpcChat_LLMError::None)
        lastFailure = MakeFailure(NpcChat_LLMError::Transport, "request failed without a diagnostic result");
    RecordFailure(cfg, requestClass, label, lastFailure);
    return lastFailure;
}

NpcChat_TransportSnapshot NpcChatTransport::GetSnapshot() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    NpcChat_TransportSnapshot snapshot;
    snapshot.totalRequests = _totalRequests;
    snapshot.succeeded = _succeeded;
    snapshot.failed = _failed;
    snapshot.rejectedCapacity = _rejectedCapacity;
    snapshot.inFlightTotal = _inFlightTotal;
    snapshot.inFlightInteractive = _inFlightInteractive;
    snapshot.inFlightBackground = _inFlightBackground;
    snapshot.inFlightGeneration = _inFlightGeneration;
    snapshot.lastError = _lastError;
    snapshot.lastHttpStatus = _lastHttpStatus;
    snapshot.lastSuccessAt = _lastSuccessAt;
    snapshot.lastFailureAt = _lastFailureAt;
    snapshot.lastErrorDetail = _lastErrorDetail;
    return snapshot;
}

void NpcChatTransport::ResetStats()
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    _totalRequests = 0;
    _succeeded = 0;
    _failed = 0;
    _rejectedCapacity = 0;
    _lastError = NpcChat_LLMError::None;
    _lastHttpStatus = 0;
    _lastSuccessAt = 0;
    _lastFailureAt = 0;
    _lastErrorDetail.clear();
    _lastLoggedErrorKey.clear();
    _nextSameErrorLogAt = 0;
    _failureSinceLastSuccess = false;
}

NpcChat_LLMResult NpcChat_CallLLM(const NpcChat_ApiConfig& cfg,
    const std::string& systemPrompt,
    const std::string& userPrompt,
    NpcChat_RequestClass requestClass,
    std::string_view label)
{
    return NpcChatTransport::Instance().Call(cfg, systemPrompt, userPrompt, requestClass, label);
}

NpcChat_TransportSnapshot NpcChat_GetTransportSnapshot()
{
    return NpcChatTransport::Instance().GetSnapshot();
}

void NpcChat_ResetTransportStats()
{
    NpcChatTransport::Instance().ResetStats();
}

NpcChat_LLMResult NpcChat_CallBackgroundLLM(const NpcChat_ApiConfig& cfg,
    const std::string& systemPrompt,
    const std::string& userPrompt,
    std::string_view label)
{
    return NpcChatTransport::Instance().Call(cfg, systemPrompt, userPrompt,
        NpcChat_RequestClass::Background, label);
}

NpcChat_LLMResult NpcChat_CallGenerationLLM(const NpcChat_ApiConfig& cfg,
    const std::string& systemPrompt,
    const std::string& userPrompt,
    std::string_view label)
{
    return NpcChatTransport::Instance().Call(cfg, systemPrompt, userPrompt,
        NpcChat_RequestClass::Generation, label);
}

NpcChat_LLMResult NpcChat_CallHealthLLM(const NpcChat_ApiConfig& cfg,
    const std::string& systemPrompt,
    const std::string& userPrompt)
{
    return NpcChatTransport::Instance().Call(cfg, systemPrompt, userPrompt,
        NpcChat_RequestClass::HealthCheck, "health-test");
}
