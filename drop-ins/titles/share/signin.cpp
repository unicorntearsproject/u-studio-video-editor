#include "signin.h"

#include <libsoup/soup.h>

#include <random>

#ifdef TITLES_HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

namespace ustudio::titles::share {

namespace {

std::string randomState()
{
    std::random_device device;
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 32; ++i)
        out += kHex[device() & 0xf];
    return out;
}

constexpr const char *kPage = "<!doctype html><meta charset=utf-8><title>U-Stu</title>"
                              "<body style=\"font-family:sans-serif;background:#120c1f;color:#ffeffb;"
                              "text-align:center;padding-top:20vh\"><h1>%s</h1><p>%s</p></body>";

#ifdef TITLES_HAVE_LIBSECRET
const SecretSchema *schema()
{
    static const SecretSchema s = {
        "com.ustudio.Share.RefreshToken",
        SECRET_SCHEMA_NONE,
        {{"service", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SecretSchemaAttributeType(0)}},
        0,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr};
    return &s;
}
#endif

} // namespace

LoopbackSignIn::LoopbackSignIn()
{
    m_server = soup_server_new(nullptr, nullptr);
    soup_server_add_handler(m_server, "/callback", &LoopbackSignIn::onCallback, this, nullptr);
    // Loopback only, IPv4 (the address the service allows), a port of the
    // system's choosing.
    GError *error = nullptr;
    if (!soup_server_listen_local(m_server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error)) {
        g_clear_error(&error);
        return;
    }
    GSList *uris = soup_server_get_uris(m_server);
    if (uris) {
        m_redirect =
            "http://127.0.0.1:" + std::to_string(g_uri_get_port(static_cast<GUri *>(uris->data))) + "/callback";
        g_slist_free_full(uris, reinterpret_cast<GDestroyNotify>(g_uri_unref));
    }
}

LoopbackSignIn::~LoopbackSignIn()
{
    soup_server_disconnect(m_server);
    g_object_unref(m_server);
}

std::string LoopbackSignIn::start(const std::string &base, std::function<void(std::string, std::string)> done)
{
    m_done = std::move(done);
    m_pkce = makePkce();
    m_state = randomState();
    return authorizeUrl(base, kClientId, m_redirect, m_pkce.challenge, m_state);
}

void LoopbackSignIn::received(const std::string &code, const std::string &state, const std::string &error)
{
    if (!m_done)
        return; // not signing in: whatever arrived isn't ours
    auto done = std::move(m_done);
    m_done = nullptr;
    if (!error.empty())
        done({}, "the sign-in was refused (" + error + ")");
    else if (state != m_state || code.empty())
        done({}, "the sign-in answer isn't for this sign-in");
    else
        done(code, {});
}

void LoopbackSignIn::onCallback(SoupServer *, SoupServerMessage *message, const char *, GHashTable *query,
                                gpointer self)
{
    const auto value = [query](const char *name) {
        const char *v = query ? static_cast<const char *>(g_hash_table_lookup(query, name)) : nullptr;
        return std::string(v ? v : "");
    };
    const std::string error = value("error");
    gchar *page = g_strdup_printf(kPage, error.empty() ? "Signed in" : "Not signed in",
                                  error.empty() ? "You can close this tab and go back to U-Stu." : "Go back to U-Stu.");
    soup_server_message_set_status(message, 200, nullptr);
    soup_server_message_set_response(message, "text/html; charset=utf-8", SOUP_MEMORY_TAKE, page, strlen(page));
    static_cast<LoopbackSignIn *>(self)->received(value("code"), value("state"), error);
}

std::string loadRefreshToken(const std::string &base)
{
#ifdef TITLES_HAVE_LIBSECRET
    gchar *token = secret_password_lookup_sync(schema(), nullptr, nullptr, "service", base.c_str(), nullptr);
    const std::string out = token ? token : "";
    secret_password_free(token);
    return out;
#else
    (void)base;
    return {};
#endif
}

void storeRefreshToken(const std::string &base, const std::string &token)
{
#ifdef TITLES_HAVE_LIBSECRET
    secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, "U-Stu template sharing", token.c_str(), nullptr,
                               nullptr, "service", base.c_str(), nullptr);
#else
    (void)base;
    (void)token;
#endif
}

void forgetRefreshToken(const std::string &base)
{
#ifdef TITLES_HAVE_LIBSECRET
    secret_password_clear_sync(schema(), nullptr, nullptr, "service", base.c_str(), nullptr);
#else
    (void)base;
#endif
}

} // namespace ustudio::titles::share
