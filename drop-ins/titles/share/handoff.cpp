#include "handoff.h"

#include "package/archive.h"

#include <gio/gio.h>

namespace ustudio::titles::share {

namespace {

bool editorRunning(GDBusConnection *bus, const std::string &editor)
{
    GVariant *reply = g_dbus_connection_call_sync(
        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
        g_variant_new("(s)", editor.c_str()), G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
    if (!reply)
        return false;
    gboolean owned = FALSE;
    g_variant_get(reply, "(b)", &owned);
    g_variant_unref(reply);
    return owned;
}

} // namespace

std::expected<std::string, std::string> handOff(const std::string &packPath, const std::string &library,
                                                const std::string &editor)
{
    if (GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr)) {
        const bool running = editorRunning(bus, editor);
        bool sent = false;
        if (running) {
            GVariantBuilder parameters;
            g_variant_builder_init(&parameters, G_VARIANT_TYPE("av"));
            g_variant_builder_add(&parameters, "v", g_variant_new_string(packPath.c_str()));
            GError *error = nullptr;
            GVariant *reply = g_dbus_connection_call_sync(
                bus, editor.c_str(), "/com/ustudio/VideoEditor", "org.freedesktop.Application", "ActivateAction",
                g_variant_new("(sava{sv})", "install-template-pack", &parameters, nullptr), nullptr,
                G_DBUS_CALL_FLAGS_NO_AUTO_START, 30000, nullptr, &error);
            if (reply) {
                g_variant_unref(reply);
                sent = true;
            }
            g_clear_error(&error);
        }
        g_object_unref(bus);
        if (sent)
            return std::string("Sent to U-Stu: it installs the pack in My Templates.");
    }
    auto installed = pack::openPackage(packPath, library, pack::Replace::IfNewer);
    if (!installed)
        return std::unexpected(installed.error());
    return "Installed “" + installed->manifest.title + "” in My Templates.";
}

} // namespace ustudio::titles::share
