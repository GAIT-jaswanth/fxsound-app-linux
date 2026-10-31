/*
FxSound - StatusNotifierItem tray icon for Linux.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/
/*
 * A tray icon over D-Bus (the StatusNotifierItem + com.canonical.dbusmenu
 * protocols), as used by Ubuntu's AppIndicator extension, KDE Plasma,
 * Cinnamon, XFCE's status notifier plugin, waybar and others.
 *
 * Why not JUCE's SystemTrayIconComponent: that is an XEmbed icon, i.e. an X11
 * window the desktop shell embeds into its panel. On GNOME the shell itself
 * embeds it, and GNOME Shell treats an X error during embedding as fatal: a
 * misbehaving icon takes down the whole session. JUCE also re-docks the same
 * window every time the icon image changes, which triggers exactly that.
 * A StatusNotifierItem only sends data (pixels, labels) over D-Bus, so the
 * shell never touches an FxSound window.
 *
 * This class has no JUCE dependency. It runs its own GLib main loop on a
 * background thread; every Callbacks function is called on that thread, so
 * the owner must hop to its UI thread. All setters may be called from any
 * thread.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class FxStatusNotifierItem
{
public:
    struct Pixmap
    {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> argb;   // width*height pixels, ARGB32 in network byte order
    };

    struct MenuItem
    {
        std::string label;
        bool enabled = true;
        bool separator = false;
        bool checked = false;        // shown with a check mark
        int action = -1;             // passed to Callbacks::menuAction when clicked; -1 = none
        std::vector<MenuItem> children;
    };

    struct Callbacks
    {
        std::function<void(int x, int y)> activate;     // primary click on the icon
        std::function<void(int action)> menuAction;     // a menu item was clicked
        std::function<void()> menuAboutToShow;          // menu is opening: call setMenu() with fresh items
        std::function<void(bool)> hostChanged;          // a tray host appeared (true) or went away (false)
    };

    FxStatusNotifierItem(const std::string& id, const std::string& title, Callbacks callbacks);
    ~FxStatusNotifierItem();

    // Waits until it is known whether a tray host (StatusNotifierWatcher) has
    // accepted the icon. Returns true if the icon is shown. If there is no host
    // yet, the icon keeps waiting and registers as soon as one appears.
    bool waitForHost(int timeout_ms);
    bool isRegistered() const;

    void setIcon(std::vector<Pixmap> sizes);
    void setToolTip(const std::string& title, const std::string& text);
    void setMenu(std::vector<MenuItem> items);

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};
