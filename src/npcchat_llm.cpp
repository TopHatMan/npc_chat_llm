#ifdef _WIN32
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "npcchat_llm.h"

// Rename the httplib namespace so this module can be loaded alongside
// mod-playerbots-characters (which does the same with pbc_httplib) without
// an ODR violation. httplib.h is header-only; if your build can't find it,
// point the include path at the copy PBC already uses in its source tree.
#define httplib npcchat_httplib
#include <httplib.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <regex>
#include <thread>
#include <vector>
#include <utility>

using json = nlohmann::json;

static std::string Trim(std::string s)
{
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

// One synchronous POST to {baseUrl}/chat/completions. Returns body or "".
static std::string HttpPost(const std::string& url,
                            const std::string& body,
                            const std::string& apiKey,
                            int timeoutSec)
{
    try
    {
        static const std::regex urlRe(R"(^(https?)://([^:/]+)(?::(\d+))?(/.*)?$)");
        std::smatch m;
        if (!std::regex_match(url, m, urlRe))
            return "";

        std::string proto = m[1].str();
        std::string host  = m[2].str();
        std::string path  = m[4].matched ? m[4].str() : "/";
        int port = proto == "https" ? 443 : 80;
        if (m[3].matched) port = std::stoi(m[3].str());

        httplib::Headers headers = {
            {"Accept", "application/json"},
            {"User-Agent", "AzerothCore-NpcChat/1.0"},
        };
        if (!apiKey.empty())
            headers.emplace("Authorization", "Bearer " + apiKey);

        httplib::Result res;
        if (proto == "https")
        {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
            httplib::SSLClient cli(host, port);
            cli.enable_server_certificate_verification(false);
            cli.set_connection_timeout(timeoutSec);
            cli.set_read_timeout(timeoutSec);
            cli.set_write_timeout(timeoutSec);
            res = cli.Post(path, headers, body, "application/json");
#else
            return ""; // built without OpenSSL — https unavailable
#endif
        }
        else
        {
            httplib::Client cli(host, port);
            cli.set_connection_timeout(timeoutSec);
            cli.set_read_timeout(timeoutSec);
            cli.set_write_timeout(timeoutSec);
            res = cli.Post(path, headers, body, "application/json");
        }

        if (!res || res->status != 200)
            return "";
        return res->body;
    }
    catch (const std::exception&)
    {
        return "";
    }
}

NpcChat_LLMResult NpcChat_CallLLM(const NpcChat_ApiConfig& cfg,
                                  const std::string& systemPrompt,
                                  const std::string& userPrompt)
{
    NpcChat_LLMResult result;

    std::string url = cfg.baseUrl;
    if (!url.empty() && url.back() == '/')
        url.pop_back();
    url += "/chat/completions";

    json body;
    body["model"]       = cfg.model;
    body["temperature"] = cfg.temperature;
    if (cfg.maxTokens > 0)
        body["max_tokens"] = cfg.maxTokens;

    json messages = json::array();
    if (!systemPrompt.empty())
        messages.push_back({ {"role", "system"}, {"content", systemPrompt} });
    messages.push_back({ {"role", "user"}, {"content", userPrompt} });
    body["messages"] = messages;

    std::string bodyStr = body.dump();

    // Splice in raw extra params (single quotes -> double), same trick as PBC.
    if (!cfg.extraParams.empty())
    {
        std::string extra = cfg.extraParams;
        std::replace(extra.begin(), extra.end(), '\'', '"');
        if (!bodyStr.empty() && bodyStr.back() == '}')
        {
            bodyStr.pop_back();
            bodyStr += "," + extra + "}";
        }
    }

    constexpr int MAX_ATTEMPTS = 2;
    for (int attempt = 1; attempt <= MAX_ATTEMPTS; ++attempt)
    {
        if (attempt > 1)
            std::this_thread::sleep_for(std::chrono::seconds(2));

        std::string resp = HttpPost(url, bodyStr, cfg.apiKey, cfg.timeoutSec);
        if (resp.empty())
            continue;

        try
        {
            json j = json::parse(resp);
            if (j.contains("error"))
                continue;

            std::string text = j["choices"][0]["message"]["content"].get<std::string>();
            text = Trim(text);

            // Strip wrapping quotes some models add.
            if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                text = text.substr(1, text.size() - 2);

            // Collapse newlines so the reply fits one chat bubble.
            for (char& c : text)
                if (c == '\n' || c == '\r') c = ' ';

            result.success = true;
            result.text    = Trim(text);
            return result;
        }
        catch (const std::exception&)
        {
            // fall through to retry
        }
    }

    return result;
}
