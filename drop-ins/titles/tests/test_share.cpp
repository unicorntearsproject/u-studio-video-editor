// u-studio-share's client (T7) against tools/share_mock.py, doc 21's API
// run locally: browsing, a download through a signed URL checked again
// here (a tampered one refused, nothing left), sign-in with PKCE, publish,
// and no request at all without a call (the mock's request log).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/media/utf8_path.h"
#include "package/archive.h"
#include "share/client.h"
#include "share/handoff.h"
#include "share/signin.h"

#include <gio/gio.h>
#include <libsoup/soup.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using namespace ustudio;
using namespace ustudio::titles;
namespace fs = std::filesystem;

namespace {

fs::path scratch()
{
    static const fs::path dir =
        fs::temp_directory_path() /
        ("ustudio-titles-share-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    return dir;
}

bool run(std::vector<std::string> args)
{
    std::vector<const char *> argv;
    for (const std::string &a : args)
        argv.push_back(a.c_str());
    argv.push_back(nullptr);
    GSubprocess *p = g_subprocess_newv(argv.data(), G_SUBPROCESS_FLAGS_STDOUT_SILENCE, nullptr);
    if (!p)
        return false;
    const bool ok = g_subprocess_wait_check(p, nullptr, nullptr);
    g_object_unref(p);
    return ok;
}

// tools/share_mock.py on a free port, seeded, stopped when this goes.
struct Mock
{
    GSubprocess *process = nullptr;
    std::string base;
    fs::path log;

    explicit Mock(const fs::path &seed, bool corrupt = false)
    {
        log = scratch() / (corrupt ? "corrupt.log" : "requests.log");
        std::vector<std::string> args = {"python3", TITLES_SHARE_MOCK, "--seed", seed.string(), "--log", log.string()};
        if (corrupt)
            args.push_back("--corrupt");
        std::vector<const char *> argv;
        for (const std::string &a : args)
            argv.push_back(a.c_str());
        argv.push_back(nullptr);
        process = g_subprocess_newv(argv.data(), G_SUBPROCESS_FLAGS_STDOUT_PIPE, nullptr);
        REQUIRE(process);
        GDataInputStream *out = g_data_input_stream_new(g_subprocess_get_stdout_pipe(process));
        gchar *line = g_data_input_stream_read_line_utf8(out, nullptr, nullptr, nullptr);
        REQUIRE(line);
        const std::string text = line;
        g_free(line);
        g_object_unref(out);
        REQUIRE(text.starts_with("listening on "));
        base = text.substr(13);
    }
    ~Mock()
    {
        g_subprocess_force_exit(process);
        g_subprocess_wait(process, nullptr, nullptr);
        g_object_unref(process);
    }
    std::string requests() const
    {
        std::ifstream in(log);
        std::stringstream s;
        s << in.rdbuf();
        return s.str();
    }
};

fs::path seed()
{
    static const fs::path dir = [] {
        const fs::path d = scratch() / "seed";
        fs::create_directories(d);
        REQUIRE(run({"python3", TITLES_MAKE_TEST_PACK, (d / "smoke.zip").string()}));
        return d;
    }();
    return dir;
}

} // namespace

TEST_CASE("browse and download: the signed URL's file is checked again here")
{
    Mock mock(seed());
    share::Client client(mock.base);
    CHECK(mock.requests().empty()); // a client alone reaches nothing

    auto packs = client.list("", "");
    REQUIRE_MESSAGE(packs.has_value(), (packs ? "" : packs.error()));
    REQUIRE(packs->size() == 1);
    CHECK((*packs)[0].id == "test/smoke-pack");
    CHECK((*packs)[0].latestVersion == "1.0.0");
    CHECK(client.list("no such thing", "")->empty());

    auto details = client.pack("test/smoke-pack");
    REQUIRE(details.has_value());
    CHECK(details->title == "Smoke test pack");
    REQUIRE(details->versions.size() == 1);
    CHECK(details->templates.size() == 1);
    CHECK_FALSE(client.pack("nobody/nothing").has_value());

    auto path = client.download("test/smoke-pack", "1.0.0", core::utf8String(scratch() / "downloads"));
    REQUIRE_MESSAGE(path.has_value(), (path ? "" : path.error()));
    CHECK(fs::exists(core::pathFromUtf8(*path)));
    CHECK(pack::inspectPackage(*path).has_value());
    CHECK(mock.requests() ==
          "GET /v1/packs\nGET /v1/packs\nGET /v1/packs/test%2Fsmoke-pack\n"
          "GET /v1/packs/nobody%2Fnothing\nPOST /v1/packs/test%2Fsmoke-pack/versions/1.0.0/download\n"
          "GET /files/" +
              mock.requests().substr(mock.requests().rfind("GET /files/") + 11));
}

TEST_CASE("a tampered download is refused and leaves nothing")
{
    Mock mock(seed(), true);
    share::Client client(mock.base);
    const fs::path downloads = scratch() / "tampered";
    auto path = client.download("test/smoke-pack", "1.0.0", core::utf8String(downloads));
    REQUIRE_FALSE(path.has_value());
    CHECK(path.error().find("SHA-256") != std::string::npos);
    CHECK((!fs::exists(downloads) || fs::is_empty(downloads)));
}

TEST_CASE("sign in with PKCE, then publish; nothing publishes signed out")
{
    Mock mock(seed());
    share::Client client(mock.base);
    const std::string redirect = "http://127.0.0.1:9/callback"; // the helper's loopback listener in real use
    const share::Pkce pkce = share::makePkce();
    CHECK(pkce.verifier.size() >= 43);
    CHECK(pkce.challenge.size() == 43);
    CHECK(share::makePkce().verifier != pkce.verifier);

    // The browser's part: the sign-in page redirects back with a code.
    SoupSession *browser = soup_session_new();
    SoupMessage *page = soup_message_new(
        "GET", share::authorizeUrl(mock.base, "u-studio-share", redirect, pkce.challenge, "xyz").c_str());
    soup_message_set_flags(page, SOUP_MESSAGE_NO_REDIRECT);
    GBytes *ignored = soup_session_send_and_read(browser, page, nullptr, nullptr);
    if (ignored)
        g_bytes_unref(ignored);
    REQUIRE(soup_message_get_status(page) == 302);
    const std::string location = soup_message_headers_get_one(soup_message_get_response_headers(page), "Location");
    g_object_unref(page);
    g_object_unref(browser);
    REQUIRE(location.starts_with(redirect + "?"));
    CHECK(location.find("state=xyz") != std::string::npos);
    const size_t at = location.find("code=") + 5;
    const std::string code = location.substr(at, location.find('&', at) - at);

    // Signed out: no publishing.
    auto early = client.publish(core::utf8String(seed() / "smoke.zip"));
    REQUIRE_FALSE(early.has_value());
    CHECK(early.error() == "sign in first");

    CHECK_FALSE(client.exchangeCode("u-studio-share", code, "a-wrong-verifier-a-wrong-verifier-a-wrong-v", redirect));
    // (a refused exchange spends the code, as a real service does)
    const share::Pkce again = share::makePkce();
    SoupSession *browser2 = soup_session_new();
    SoupMessage *page2 = soup_message_new(
        "GET", share::authorizeUrl(mock.base, "u-studio-share", redirect, again.challenge, "abc").c_str());
    soup_message_set_flags(page2, SOUP_MESSAGE_NO_REDIRECT);
    GBytes *ignored2 = soup_session_send_and_read(browser2, page2, nullptr, nullptr);
    if (ignored2)
        g_bytes_unref(ignored2);
    const std::string location2 = soup_message_headers_get_one(soup_message_get_response_headers(page2), "Location");
    g_object_unref(page2);
    g_object_unref(browser2);
    const size_t at2 = location2.find("code=") + 5;
    const std::string code2 = location2.substr(at2, location2.find('&', at2) - at2);
    auto tokens = client.exchangeCode("u-studio-share", code2, again.verifier, redirect);
    REQUIRE_MESSAGE(tokens.has_value(), (tokens ? "" : tokens.error()));
    client.setAccessToken(tokens->access);
    auto who = client.me();
    REQUIRE(who.has_value());
    CHECK(who->slug == "tester");
    auto renewed = client.refresh("u-studio-share", tokens->refresh);
    REQUIRE(renewed.has_value());
    client.setAccessToken(renewed->access);

    // Publish a newer version; the service lists it.
    const fs::path newer = scratch() / "newer.zip";
    REQUIRE(run({"python3", TITLES_MAKE_TEST_PACK, newer.string(), "--version", "1.1.0"}));
    auto upload = client.publish(core::utf8String(newer));
    REQUIRE_MESSAGE(upload.has_value(), (upload ? "" : upload.error()));
    auto status = client.uploadStatus(*upload);
    REQUIRE(status.has_value());
    CHECK(status->state == share::UploadState::Published);
    CHECK((*client.list("", ""))[0].latestVersion == "1.1.0");

    // A file that isn't a valid pack never leaves the machine.
    const fs::path junk = scratch() / "junk.zip";
    std::ofstream(junk) << "not a pack";
    const std::string before = mock.requests();
    CHECK_FALSE(client.publish(core::utf8String(junk)).has_value());
    CHECK(mock.requests() == before);
}

namespace {

// What a browser does with a URL: follows it once, no further redirects;
// the status and the Location it points to.
std::pair<unsigned, std::string> visit(const std::string &url)
{
    SoupSession *browser = soup_session_new();
    SoupMessage *page = soup_message_new("GET", url.c_str());
    soup_message_set_flags(page, SOUP_MESSAGE_NO_REDIRECT);
    GBytes *body = soup_session_send_and_read(browser, page, nullptr, nullptr);
    if (body)
        g_bytes_unref(body);
    const unsigned status = soup_message_get_status(page);
    const char *location = soup_message_headers_get_one(soup_message_get_response_headers(page), "Location");
    std::pair<unsigned, std::string> out{status, location ? location : ""};
    g_object_unref(page);
    g_object_unref(browser);
    return out;
}

// Runs the main loop until `done`, at most ten seconds.
void pump(const bool &done)
{
    const gint64 end = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (!done && g_get_monotonic_time() < end)
        g_main_context_iteration(nullptr, FALSE);
}

} // namespace

TEST_CASE("the loopback sign-in takes the browser's redirect, only with its own state")
{
    Mock mock(seed());
    share::LoopbackSignIn signIn;
    REQUIRE(signIn.listening());
    CHECK(signIn.redirectUri().starts_with("http://127.0.0.1:"));

    bool done = false;
    std::string code, error;
    const std::string page = signIn.start(mock.base, [&](std::string c, std::string e) {
        code = std::move(c);
        error = std::move(e);
        done = true;
    });
    // The browser: the sign-in page redirects to our listener, which it then
    // visits (on a thread: the listener answers from this main loop).
    const auto [status, location] = visit(page);
    REQUIRE(status == 302);
    REQUIRE(location.starts_with(signIn.redirectUri()));
    std::thread browser([location] { visit(location); });
    pump(done);
    browser.join();
    REQUIRE(done);
    CHECK(error.empty());
    REQUIRE_FALSE(code.empty());
    share::Client client(mock.base);
    auto tokens = client.exchangeCode(share::kClientId, code, signIn.pkce().verifier, signIn.redirectUri());
    CHECK_MESSAGE(tokens.has_value(), (tokens ? "" : tokens.error()));

    // Another sign-in, answered with the wrong state: refused.
    done = false;
    signIn.start(mock.base, [&](std::string c, std::string e) {
        code = std::move(c);
        error = std::move(e);
        done = true;
    });
    std::thread forged([&] { visit(signIn.redirectUri() + "?code=stolen&state=not-ours"); });
    pump(done);
    forged.join();
    REQUIRE(done);
    CHECK(code.empty());
    CHECK(error.find("isn't for this sign-in") != std::string::npos);

    // Nothing waiting: a stray callback does nothing.
    std::thread stray([&] { visit(signIn.redirectUri() + "?code=x&state=y"); });
    done = false;
    const gint64 end = g_get_monotonic_time() + G_USEC_PER_SEC / 2;
    while (g_get_monotonic_time() < end)
        g_main_context_iteration(nullptr, FALSE);
    stray.join();
    CHECK_FALSE(done);
}

TEST_CASE("hand-off without a running editor installs into the template library")
{
    Mock mock(seed());
    // A scratch library, and an editor name nobody owns: this test must
    // reach neither a running U-Stu nor the user's templates.
    const fs::path library = scratch() / "library";
    auto path = share::Client(mock.base).download("test/smoke-pack", "1.0.0", core::utf8String(scratch() / "dl"));
    REQUIRE(path.has_value());
    auto result = share::handOff(*path, core::utf8String(library), "com.ustudio.Test.NoSuchEditor");
    REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error()));
    CHECK(result->find("Installed") != std::string::npos);
    CHECK(fs::exists(library / "packs" / "test--smoke-pack" / "pack.xml"));
    fs::remove_all(scratch());
}
