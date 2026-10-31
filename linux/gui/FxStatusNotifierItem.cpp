/*
FxSound - StatusNotifierItem tray icon for Linux (see FxStatusNotifierItem.h).

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/
#include "FxStatusNotifierItem.h"

#include <gio/gio.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
constexpr const char* kItemPath = "/StatusNotifierItem";
constexpr const char* kMenuPath = "/MenuBar";
constexpr const char* kItemIface = "org.kde.StatusNotifierItem";
constexpr const char* kMenuIface = "com.canonical.dbusmenu";
constexpr const char* kWatcherName = "org.kde.StatusNotifierWatcher";
constexpr const char* kWatcherPath = "/StatusNotifierWatcher";
constexpr int kAboutToShowTimeoutMs = 300;   // max wait for fresh menu items when a menu opens

constexpr const char* kIntrospection = R"XML(
<node>
  <interface name='org.kde.StatusNotifierItem'>
    <property name='Category' type='s' access='read'/>
    <property name='Id' type='s' access='read'/>
    <property name='Title' type='s' access='read'/>
    <property name='Status' type='s' access='read'/>
    <property name='WindowId' type='i' access='read'/>
    <property name='IconName' type='s' access='read'/>
    <property name='IconPixmap' type='a(iiay)' access='read'/>
    <property name='OverlayIconName' type='s' access='read'/>
    <property name='OverlayIconPixmap' type='a(iiay)' access='read'/>
    <property name='AttentionIconName' type='s' access='read'/>
    <property name='AttentionIconPixmap' type='a(iiay)' access='read'/>
    <property name='AttentionMovieName' type='s' access='read'/>
    <property name='IconThemePath' type='s' access='read'/>
    <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>
    <property name='ItemIsMenu' type='b' access='read'/>
    <property name='Menu' type='o' access='read'/>
    <method name='ContextMenu'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>
    <method name='Activate'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>
    <method name='SecondaryActivate'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>
    <method name='Scroll'><arg name='delta' type='i' direction='in'/><arg name='orientation' type='s' direction='in'/></method>
    <signal name='NewTitle'/>
    <signal name='NewIcon'/>
    <signal name='NewAttentionIcon'/>
    <signal name='NewOverlayIcon'/>
    <signal name='NewToolTip'/>
    <signal name='NewStatus'><arg name='status' type='s'/></signal>
  </interface>
  <interface name='com.canonical.dbusmenu'>
    <property name='Version' type='u' access='read'/>
    <property name='TextDirection' type='s' access='read'/>
    <property name='Status' type='s' access='read'/>
    <property name='IconThemePath' type='as' access='read'/>
    <method name='GetLayout'>
      <arg name='parentId' type='i' direction='in'/>
      <arg name='recursionDepth' type='i' direction='in'/>
      <arg name='propertyNames' type='as' direction='in'/>
      <arg name='revision' type='u' direction='out'/>
      <arg name='layout' type='(ia{sv}av)' direction='out'/>
    </method>
    <method name='GetGroupProperties'>
      <arg name='ids' type='ai' direction='in'/>
      <arg name='propertyNames' type='as' direction='in'/>
      <arg name='properties' type='a(ia{sv})' direction='out'/>
    </method>
    <method name='GetProperty'>
      <arg name='id' type='i' direction='in'/>
      <arg name='name' type='s' direction='in'/>
      <arg name='value' type='v' direction='out'/>
    </method>
    <method name='Event'>
      <arg name='id' type='i' direction='in'/>
      <arg name='eventId' type='s' direction='in'/>
      <arg name='data' type='v' direction='in'/>
      <arg name='timestamp' type='u' direction='in'/>
    </method>
    <method name='EventGroup'>
      <arg name='events' type='a(isvu)' direction='in'/>
      <arg name='idErrors' type='ai' direction='out'/>
    </method>
    <method name='AboutToShow'>
      <arg name='id' type='i' direction='in'/>
      <arg name='needUpdate' type='b' direction='out'/>
    </method>
    <method name='AboutToShowGroup'>
      <arg name='ids' type='ai' direction='in'/>
      <arg name='updatesNeeded' type='ai' direction='out'/>
      <arg name='idErrors' type='ai' direction='out'/>
    </method>
    <signal name='ItemsPropertiesUpdated'>
      <arg name='updatedProps' type='a(ia{sv})'/>
      <arg name='removedProps' type='a(ias)'/>
    </signal>
    <signal name='LayoutUpdated'><arg name='revision' type='u'/><arg name='parent' type='i'/></signal>
    <signal name='ItemActivationRequested'><arg name='id' type='i'/><arg name='timestamp' type='u'/></signal>
  </interface>
</node>)XML";

std::string validUtf8(const std::string& s)
{
    if (g_utf8_validate(s.data(), (gssize)s.size(), nullptr))
        return s;
    gchar* fixed = g_utf8_make_valid(s.data(), (gssize)s.size());
    std::string out(fixed);
    g_free(fixed);
    return out;
}

// dbusmenu labels use '_' for mnemonics; a literal underscore is "__".
std::string menuLabel(const std::string& s)
{
    std::string out;
    for (char c : validUtf8(s))
    {
        out += c;
        if (c == '_')
            out += '_';
    }
    return out;
}

struct MenuNode
{
    int parent = 0;
    std::vector<int> children;
    std::string label;
    bool enabled = true;
    bool separator = false;
    bool checked = false;
    int action = -1;

    bool sameLook(const MenuNode& o) const
    {
        return parent == o.parent && children == o.children && label == o.label && enabled == o.enabled
            && separator == o.separator && checked == o.checked;
    }
};
} // namespace

struct FxStatusNotifierItem::Impl
{
    Impl(const std::string& id_, const std::string& title_, Callbacks cb_)
        : id(validUtf8(id_)), title(validUtf8(title_)), cb(std::move(cb_))
    {
        nodes.resize(1);   // node 0 is the root
        ctx = g_main_context_new();
        loop = g_main_loop_new(ctx, FALSE);
        cancellable = g_cancellable_new();
    }

    ~Impl()
    {
        g_object_unref(cancellable);
        g_main_loop_unref(loop);
        g_main_context_unref(ctx);   // frees anything still posted
    }

    // ---- thread plumbing -------------------------------------------------
    void post(std::function<void()> fn)
    {
        auto* heap = new std::function<void()>(std::move(fn));
        g_main_context_invoke_full(
            ctx, G_PRIORITY_DEFAULT,
            [](gpointer p) -> gboolean { (*static_cast<std::function<void()>*>(p))(); return G_SOURCE_REMOVE; },
            heap, [](gpointer p) { delete static_cast<std::function<void()>*>(p); });
    }

    void start() { thread = std::thread([this] { run(); }); }

    void stop()
    {
        g_cancellable_cancel(cancellable);
        post([this] { g_main_loop_quit(loop); });
        if (thread.joinable())
            thread.join();
    }

    void run()
    {
        g_main_context_push_thread_default(ctx);
        GError* err = nullptr;
        conn = g_bus_get_sync(G_BUS_TYPE_SESSION, cancellable, &err);
        if (conn == nullptr)
        {
            g_clear_error(&err);
            decide();
            g_main_context_pop_thread_default(ctx);
            return;
        }

        node_info = g_dbus_node_info_new_for_xml(kIntrospection, nullptr);
        static const GDBusInterfaceVTable vtable = { &Impl::onMethod, &Impl::onGetProperty, nullptr, { nullptr } };
        item_reg = g_dbus_connection_register_object(conn, kItemPath,
            g_dbus_node_info_lookup_interface(node_info, kItemIface), &vtable, this, nullptr, nullptr);
        menu_reg = g_dbus_connection_register_object(conn, kMenuPath,
            g_dbus_node_info_lookup_interface(node_info, kMenuIface), &vtable, this, nullptr, nullptr);

        bus_name = "org.kde.StatusNotifierItem-" + std::to_string((long)getpid()) + "-1";
        own_id = g_bus_own_name_on_connection(conn, bus_name.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, gpointer p) { auto* self = static_cast<Impl*>(p); self->name_owned = true; self->tryRegister(); },
            [](GDBusConnection*, const gchar*, gpointer p) { auto* self = static_cast<Impl*>(p); self->name_owned = false; self->setRegistered(false); },
            this, nullptr);
        watch_id = g_bus_watch_name_on_connection(conn, kWatcherName, G_BUS_NAME_WATCHER_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, gpointer p) { auto* self = static_cast<Impl*>(p); self->watcher_present = true; self->tryRegister(); },
            [](GDBusConnection*, const gchar*, gpointer p) { auto* self = static_cast<Impl*>(p); self->watcher_present = false; self->setRegistered(false); },
            this, nullptr);

        g_main_loop_run(loop);

        g_bus_unwatch_name(watch_id);
        g_bus_unown_name(own_id);
        g_dbus_connection_unregister_object(conn, item_reg);
        g_dbus_connection_unregister_object(conn, menu_reg);
        finishAboutToShow(false);
        while (g_main_context_iteration(ctx, FALSE)) {}   // let cancelled calls finish while we still exist
        g_dbus_connection_flush_sync(conn, nullptr, nullptr);
        g_dbus_node_info_unref(node_info);
        g_object_unref(conn);
        g_main_context_pop_thread_default(ctx);
    }

    // ---- registration with the tray host ---------------------------------
    void tryRegister()
    {
        if (!name_owned || !watcher_present)
            return;
        g_dbus_connection_call(conn, kWatcherName, kWatcherPath, kWatcherName, "RegisterStatusNotifierItem",
            g_variant_new("(s)", bus_name.c_str()), nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, cancellable,
            [](GObject* source, GAsyncResult* res, gpointer p) {
                GError* err = nullptr;
                GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
                if (err != nullptr && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                {
                    g_error_free(err);
                    return;   // shutting down; p may be about to go away
                }
                static_cast<Impl*>(p)->setRegistered(reply != nullptr);
                if (reply != nullptr)
                    g_variant_unref(reply);
                g_clear_error(&err);
            },
            this);
    }

    void setRegistered(bool value)
    {
        bool was = registered.exchange(value);
        if (was != value && cb.hostChanged)
            cb.hostChanged(value);
        decide();
    }

    void decide()
    {
        std::lock_guard<std::mutex> lock(mutex);
        decided = true;
        cv.notify_all();
    }

    void emit(const char* iface, const char* path, const char* signal, GVariant* args = nullptr)
    {
        if (conn != nullptr)
            g_dbus_connection_emit_signal(conn, nullptr, path, iface, signal, args, nullptr);
        else if (args != nullptr)
            g_variant_unref(g_variant_ref_sink(args));
    }

    // ---- icon / tooltip --------------------------------------------------
    static GVariant* pixmapsVariant(const std::vector<Pixmap>& pixmaps)
    {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(iiay)"));
        for (const auto& p : pixmaps)
        {
            if (p.width <= 0 || p.height <= 0 || p.argb.size() != (size_t)p.width * p.height * 4)
                continue;
            GVariant* bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, p.argb.data(), p.argb.size(), 1);
            g_variant_builder_add(&b, "(ii@ay)", p.width, p.height, bytes);
        }
        return g_variant_builder_end(&b);
    }

    GVariant* toolTipVariant() const
    {
        return g_variant_new("(s@a(iiay)ss)", "", pixmapsVariant({}), tip_title.c_str(), tip_text.c_str());
    }

    // ---- menu ------------------------------------------------------------
    void applyMenu(const std::vector<MenuItem>& items)
    {
        std::vector<MenuNode> fresh(1);
        addNodes(fresh, 0, items);

        bool same = fresh.size() == nodes.size();
        for (size_t i = 0; same && i < fresh.size(); i++)
            same = fresh[i].sameLook(nodes[i]);

        nodes = std::move(fresh);   // always take the new actions
        if (!same)
        {
            revision++;
            emit(kMenuIface, kMenuPath, "LayoutUpdated", g_variant_new("(ui)", revision, 0));
        }
        finishAboutToShow(!same);
    }

    static void addNodes(std::vector<MenuNode>& out, int parent, const std::vector<MenuItem>& items)
    {
        for (const auto& item : items)
        {
            int id = (int)out.size();
            MenuNode n;
            n.parent = parent;
            n.label = menuLabel(item.label);
            n.enabled = item.enabled;
            n.separator = item.separator;
            n.checked = item.checked;
            n.action = item.action;
            out.push_back(std::move(n));
            out[parent].children.push_back(id);
            addNodes(out, id, item.children);
        }
    }

    bool validId(int id) const { return id >= 0 && id < (int)nodes.size(); }

    static bool wanted(GVariant* names, const char* key)
    {
        if (names == nullptr || g_variant_n_children(names) == 0)
            return true;
        GVariantIter it;
        const gchar* name;
        g_variant_iter_init(&it, names);
        while (g_variant_iter_next(&it, "&s", &name))
            if (g_strcmp0(name, key) == 0)
                return true;
        return false;
    }

    GVariant* nodeProps(int id, GVariant* names) const
    {
        const auto& n = nodes[id];
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        auto add = [&](const char* key, GVariant* value) {
            if (wanted(names, key))
                g_variant_builder_add(&b, "{sv}", key, value);
            else
                g_variant_unref(g_variant_ref_sink(value));
        };
        if (id == 0)
        {
            add("children-display", g_variant_new_string("submenu"));
        }
        else if (n.separator)
        {
            add("type", g_variant_new_string("separator"));
        }
        else
        {
            add("label", g_variant_new_string(n.label.c_str()));
            add("enabled", g_variant_new_boolean(n.enabled));
            add("visible", g_variant_new_boolean(TRUE));
            if (n.checked)
            {
                add("toggle-type", g_variant_new_string("checkmark"));
                add("toggle-state", g_variant_new_int32(1));
            }
            if (!n.children.empty())
                add("children-display", g_variant_new_string("submenu"));
        }
        return g_variant_builder_end(&b);
    }

    GVariant* layout(int id, int depth, GVariant* names) const
    {
        GVariantBuilder children;
        g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
        if (depth != 0)
            for (int c : nodes[id].children)
                g_variant_builder_add(&children, "v", layout(c, depth > 0 ? depth - 1 : -1, names));
        return g_variant_new("(i@a{sv}@av)", id, nodeProps(id, names), g_variant_builder_end(&children));
    }

    void activateMenuItem(int id)
    {
        if (validId(id) && nodes[id].action >= 0 && nodes[id].enabled && cb.menuAction)
            cb.menuAction(nodes[id].action);
    }

    void requestMenuRefresh()
    {
        if (cb.menuAboutToShow)
            cb.menuAboutToShow();
    }

    void finishAboutToShow(bool changed)
    {
        for (auto* inv : pending_about_to_show)
            g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", changed));
        pending_about_to_show.clear();
        if (about_to_show_timeout != 0)
        {
            g_source_remove(about_to_show_timeout);
            about_to_show_timeout = 0;
        }
    }

    // ---- D-Bus handlers --------------------------------------------------
    static void onMethod(GDBusConnection*, const gchar*, const gchar*, const gchar* iface, const gchar* method,
                         GVariant* params, GDBusMethodInvocation* inv, gpointer p)
    {
        auto* self = static_cast<Impl*>(p);
        if (g_strcmp0(iface, kItemIface) == 0)
        {
            if (g_strcmp0(method, "Activate") == 0)
            {
                gint x = 0, y = 0;
                g_variant_get(params, "(ii)", &x, &y);
                if (self->cb.activate)
                    self->cb.activate(x, y);
            }
            // ContextMenu/SecondaryActivate/Scroll: hosts show the dbusmenu instead.
            g_dbus_method_invocation_return_value(inv, nullptr);
            return;
        }

        if (g_strcmp0(method, "GetLayout") == 0)
        {
            gint parent = 0, depth = -1;
            GVariant* names = nullptr;
            g_variant_get(params, "(ii@as)", &parent, &depth, &names);
            if (!self->validId(parent))
                g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.InvalidArgs", "unknown menu id");
            else
                g_dbus_method_invocation_return_value(inv,
                    g_variant_new("(u@(ia{sv}av))", self->revision, self->layout(parent, depth, names)));
            g_variant_unref(names);
        }
        else if (g_strcmp0(method, "GetGroupProperties") == 0)
        {
            GVariant* ids = nullptr;
            GVariant* names = nullptr;
            g_variant_get(params, "(@ai@as)", &ids, &names);
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE("a(ia{sv})"));
            auto add = [&](int id) { g_variant_builder_add(&b, "(i@a{sv})", id, self->nodeProps(id, names)); };
            if (g_variant_n_children(ids) == 0)
            {
                for (int id = 0; id < (int)self->nodes.size(); id++)
                    add(id);
            }
            else
            {
                GVariantIter it;
                gint32 id;
                g_variant_iter_init(&it, ids);
                while (g_variant_iter_next(&it, "i", &id))
                    if (self->validId(id))
                        add(id);
            }
            g_dbus_method_invocation_return_value(inv, g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&b)));
            g_variant_unref(ids);
            g_variant_unref(names);
        }
        else if (g_strcmp0(method, "GetProperty") == 0)
        {
            gint id = 0;
            const gchar* name = nullptr;
            g_variant_get(params, "(i&s)", &id, &name);
            GVariant* value = nullptr;
            if (self->validId(id))
            {
                GVariant* props = self->nodeProps(id, nullptr);
                value = g_variant_lookup_value(props, name, nullptr);
                g_variant_unref(g_variant_ref_sink(props));
            }
            if (value == nullptr)
                g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.InvalidArgs", "unknown property");
            else
            {
                g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", value));
                g_variant_unref(value);
            }
        }
        else if (g_strcmp0(method, "Event") == 0)
        {
            gint id = 0;
            const gchar* event = nullptr;
            g_variant_get(params, "(i&svu)", &id, &event, nullptr, nullptr);
            if (g_strcmp0(event, "clicked") == 0)
                self->activateMenuItem(id);
            else if (g_strcmp0(event, "opened") == 0 && id == 0)
                self->requestMenuRefresh();
            g_dbus_method_invocation_return_value(inv, nullptr);
        }
        else if (g_strcmp0(method, "EventGroup") == 0)
        {
            GVariantIter* it = nullptr;
            g_variant_get(params, "(a(isvu))", &it);
            GVariantBuilder errors;
            g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
            gint id;
            const gchar* event;
            while (g_variant_iter_next(it, "(i&svu)", &id, &event, nullptr, nullptr))
            {
                if (!self->validId(id))
                    g_variant_builder_add(&errors, "i", id);
                else if (g_strcmp0(event, "clicked") == 0)
                    self->activateMenuItem(id);
            }
            g_variant_iter_free(it);
            g_dbus_method_invocation_return_value(inv, g_variant_new("(@ai)", g_variant_builder_end(&errors)));
        }
        else if (g_strcmp0(method, "AboutToShow") == 0)
        {
            gint id = 0;
            g_variant_get(params, "(i)", &id);
            if (id != 0)   // submenus are refreshed together with the root
            {
                g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", FALSE));
                return;
            }
            // Answer once the owner has sent fresh items (or after a short timeout).
            self->pending_about_to_show.push_back(inv);
            if (self->about_to_show_timeout == 0)
            {
                self->about_to_show_timeout = g_timeout_add(kAboutToShowTimeoutMs, [](gpointer q) -> gboolean {
                    auto* me = static_cast<Impl*>(q);
                    me->about_to_show_timeout = 0;
                    me->finishAboutToShow(false);
                    return G_SOURCE_REMOVE;
                }, self);
                self->requestMenuRefresh();
            }
        }
        else if (g_strcmp0(method, "AboutToShowGroup") == 0)
        {
            self->requestMenuRefresh();   // any change arrives as LayoutUpdated
            g_dbus_method_invocation_return_value(inv, g_variant_new("(@ai@ai)",
                g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0), g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
        }
        else
        {
            g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method);
        }
    }

    static GVariant* onGetProperty(GDBusConnection*, const gchar*, const gchar*, const gchar* iface,
                                   const gchar* prop, GError** error, gpointer p)
    {
        auto* self = static_cast<Impl*>(p);
        auto is = [prop](const char* name) { return g_strcmp0(prop, name) == 0; };
        if (g_strcmp0(iface, kMenuIface) == 0)
        {
            if (is("Version")) return g_variant_new_uint32(3);
            if (is("TextDirection")) return g_variant_new_string("ltr");
            if (is("Status")) return g_variant_new_string("normal");
            if (is("IconThemePath")) return g_variant_new_strv(nullptr, 0);
        }
        else
        {
            if (is("Category")) return g_variant_new_string("ApplicationStatus");
            if (is("Id")) return g_variant_new_string(self->id.c_str());
            if (is("Title")) return g_variant_new_string(self->title.c_str());
            if (is("Status")) return g_variant_new_string("Active");
            if (is("WindowId")) return g_variant_new_int32(0);
            if (is("IconPixmap")) return pixmapsVariant(self->icon);
            if (is("OverlayIconPixmap") || is("AttentionIconPixmap")) return pixmapsVariant({});
            if (is("IconName") || is("OverlayIconName") || is("AttentionIconName") || is("AttentionMovieName")
                || is("IconThemePath"))
                return g_variant_new_string("");
            if (is("ToolTip")) return self->toolTipVariant();
            if (is("ItemIsMenu")) return g_variant_new_boolean(FALSE);
            if (is("Menu")) return g_variant_new_object_path(kMenuPath);
        }
        g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "unknown property %s", prop);
        return nullptr;
    }

    // ---- data ------------------------------------------------------------
    const std::string id;
    const std::string title;
    const Callbacks cb;

    GMainContext* ctx = nullptr;
    GMainLoop* loop = nullptr;
    GCancellable* cancellable = nullptr;
    std::thread thread;

    // Only used on the D-Bus thread:
    GDBusConnection* conn = nullptr;
    GDBusNodeInfo* node_info = nullptr;
    guint item_reg = 0, menu_reg = 0, own_id = 0, watch_id = 0;
    std::string bus_name;
    bool name_owned = false;
    bool watcher_present = false;
    std::vector<Pixmap> icon;
    std::string tip_title, tip_text;
    std::vector<MenuNode> nodes;
    guint32 revision = 1;
    std::vector<GDBusMethodInvocation*> pending_about_to_show;
    guint about_to_show_timeout = 0;

    // Shared with other threads:
    std::atomic<bool> registered { false };
    std::mutex mutex;
    std::condition_variable cv;
    bool decided = false;
};

FxStatusNotifierItem::FxStatusNotifierItem(const std::string& id, const std::string& title, Callbacks callbacks)
    : impl_(std::make_shared<Impl>(id, title, std::move(callbacks)))
{
    impl_->start();
}

FxStatusNotifierItem::~FxStatusNotifierItem()
{
    impl_->stop();
}

bool FxStatusNotifierItem::waitForHost(int timeout_ms)
{
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return impl_->decided; });
    return impl_->registered.load();
}

bool FxStatusNotifierItem::isRegistered() const
{
    return impl_->registered.load();
}

void FxStatusNotifierItem::setIcon(std::vector<Pixmap> sizes)
{
    auto* impl = impl_.get();
    impl->post([impl, sizes = std::move(sizes)]() mutable {
        impl->icon = std::move(sizes);
        impl->emit(kItemIface, kItemPath, "NewIcon");
    });
}

void FxStatusNotifierItem::setToolTip(const std::string& title, const std::string& text)
{
    auto* impl = impl_.get();
    impl->post([impl, title = validUtf8(title), text = validUtf8(text)] {
        if (impl->tip_title == title && impl->tip_text == text)
            return;
        impl->tip_title = title;
        impl->tip_text = text;
        impl->emit(kItemIface, kItemPath, "NewToolTip");
    });
}

void FxStatusNotifierItem::setMenu(std::vector<MenuItem> items)
{
    auto* impl = impl_.get();
    impl->post([impl, items = std::move(items)] { impl->applyMenu(items); });
}
