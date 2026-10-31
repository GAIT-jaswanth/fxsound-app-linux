/*
FxSound
Copyright (C) 2025  FxSound LLC

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <JuceHeader.h>
#include "FxSystemTrayView.h"
#include "../Utils/SysInfo/SysInfo.h"

// {A8E96325-5269-443C-A0D8-0D02562FE553}
#if JUCE_WINDOWS
const GUID FxSystemTrayView::trayIconGuid_ =
{ 0xa8e96325, 0x5269, 0x443c, { 0xa0, 0xd8, 0xd, 0x2, 0x56, 0x2f, 0xe5, 0x53 } };
#endif


#if JUCE_WINDOWS
FxSystemTrayView::FxSystemTrayView()
{
    FxModel::getModel().addListener(this);

    custom_notification_ = true;

    addToDesktop(0);

    HWND hWnd = (HWND)getWindowHandle();

    SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    componentWndProc_ = (WNDPROC)GetWindowLongPtr(hWnd, GWLP_WNDPROC);
    SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)wndProc);

    taskbar_created_message_ = RegisterWindowMessage(TEXT("TaskbarCreated"));

    addIcon();
}
#else
namespace
{
// XEmbed fallback for desktops without a StatusNotifierItem host. It docks
// only once: JUCE's setIconImage() asks the tray to dock the (already docked)
// window again on every call, which tray hosts handle badly - on GNOME it
// crashed the whole shell. Later image changes just repaint.
class FxXEmbedTrayIcon : public SystemTrayIconComponent
{
public:
    std::function<void(const MouseEvent&)> on_mouse_down;

    void setImage(const Image& image)
    {
        image_ = image;
        if (!docked_)
        {
            setIconImage(image, image);
            docked_ = true;
        }
        repaint();
    }

    void paint(Graphics& g) override
    {
        g.drawImage(image_, getLocalBounds().toFloat(), RectanglePlacement::centred | RectanglePlacement::onlyReduceInSize);
    }

    void mouseDown(const MouseEvent& e) override
    {
        if (on_mouse_down)
            on_mouse_down(e);
    }

private:
    Image image_;
    bool docked_ = false;
};

bool isGnomeDesktop()
{
    auto desktops = StringArray::fromTokens(SystemStats::getEnvironmentVariable("XDG_CURRENT_DESKTOP", {}), ":", {});
    for (auto& d : desktops)
        if (d.equalsIgnoreCase("GNOME"))
            return true;
    return false;
}
}

FxSystemTrayView::FxSystemTrayView()
{
    FxModel::getModel().addListener(this);
    custom_notification_ = true;

    // FXSOUND_TRAY=auto (default) | sni | xembed | none
    auto mode = SystemStats::getEnvironmentVariable("FXSOUND_TRAY", "auto").trim().toLowerCase();
    bool gnome = isGnomeDesktop();

    if (mode != "none" && mode != "xembed")
    {
        SafePointer<FxSystemTrayView> safe(this);
        FxStatusNotifierItem::Callbacks callbacks;
        callbacks.activate = [safe](int x, int y) {
            MessageManager::callAsync([safe, x, y] { if (safe != nullptr) safe->onTrayClick({ x, y }, false); });
        };
        callbacks.menuAction = [safe](int action) {
            MessageManager::callAsync([safe, action] { if (safe != nullptr) safe->runTrayMenuAction(action); });
        };
        callbacks.menuAboutToShow = [safe] {
            MessageManager::callAsync([safe] { if (safe != nullptr) safe->refreshTrayMenu(true); });
        };
        status_notifier_item_ = std::make_unique<FxStatusNotifierItem>("fxsound", "FxSound", callbacks);

        // On GNOME, keep waiting for a host even if there is none yet: the icon
        // appears as soon as the AppIndicator extension is enabled.
        if (!status_notifier_item_->waitForHost(1000) && mode == "auto" && !gnome)
            status_notifier_item_.reset();
    }

    // XEmbed only where it is safe: never on GNOME unless explicitly requested.
    if (status_notifier_item_ == nullptr && (mode == "xembed" || (mode == "auto" && !gnome)))
    {
        auto icon = std::make_unique<FxXEmbedTrayIcon>();
        icon->on_mouse_down = [this](const MouseEvent& e) { onTrayClick(e.getScreenPosition(), e.mods.isPopupMenu()); };
        xembed_icon_ = std::move(icon);
    }

    addIcon();   // also schedules the first tray menu, built once FxController is initialised
}
#endif

#if JUCE_WINDOWS
FxSystemTrayView::~FxSystemTrayView()
{
    FxModel::getModel().removeListener(this);

    HWND hWnd = (HWND)getWindowHandle();

    SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)componentWndProc_);
    SetWindowLongPtr(hWnd, GWLP_USERDATA, NULL);

    NOTIFYICONDATA nid = { sizeof(nid) };
    nid.uFlags = NIF_GUID;
    nid.guidItem = trayIconGuid_;
    Shell_NotifyIcon(NIM_DELETE, &nid);

    removeFromDesktop();
}
#else
FxSystemTrayView::~FxSystemTrayView()
{
    status_notifier_item_.reset();   // stops its D-Bus thread
    xembed_icon_.reset();
    FxModel::getModel().removeListener(this);
}
#endif

void FxSystemTrayView::modelChanged(FxModel::Event model_event)
{
    if (!FxController::getInstance().isNotificationsHidden() && model_event == FxModel::Event::Notification)
    {
        showNotification();
    }
#if ! JUCE_WINDOWS
    scheduleTrayMenuRefresh();   // presets, outputs and power appear in the tray menu
#endif
}

#if JUCE_WINDOWS
void FxSystemTrayView::setStatus(bool power, bool processing)
{
    HINSTANCE hInst = GetModuleHandle(NULL);

    String param = power ? TRANS(L"on") : TRANS(L"off");

    wchar_t tool_tip[1024];
    swprintf_s(tool_tip, String(TRANS("FxSound is %s.")).toWideCharPointer(), param.toWideCharPointer());
	wcscat_s(tool_tip, 1024, L"\n\n");
    wcscat_s(tool_tip, 1024, String(TRANS("Output: ")).toWideCharPointer());
    auto& model = FxModel::getModel();
	String output_device_name = model.getSelectedOutput().deviceFriendlyName.c_str();
	wcscat_s(tool_tip, 1024, output_device_name.toWideCharPointer());

    NOTIFYICONDATA nid = { sizeof(nid) };

    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP | NIF_GUID;
    nid.guidItem = trayIconGuid_;
    if (power)
    {
        if (processing)
        {
            if (FxTheme::getThemeMode() == FxThemeMode::Dark)
            {
                nid.hIcon = LoadIcon(hInst, L"IDI_LOGO_RED");
            }
            else
            {
                nid.hIcon = LoadIcon(hInst, L"IDI_LOGO_BLUE");
            }
        }
        else
        {
            nid.hIcon = LoadIcon(hInst, L"IDI_LOGO_WHITE");
        }
    }
    else
    {
        nid.hIcon = LoadIcon(hInst, L"IDI_LOGO_GRAY");
    }

    if (nid.hIcon == NULL)
    {
        return;
    }

    lstrcpy(nid.szTip, tool_tip);

    Shell_NotifyIcon(NIM_MODIFY, &nid);
}
#else
// Renders the tray icon from the app's logo SVGs (Windows uses .ico resources:
// red/blue when processing, white when on but idle, gray when off).
Image FxSystemTrayView::makeIcon(bool power, bool processing, int icon_size)
{
    const char* svg = BinaryData::logowhite_svg;
    int size = BinaryData::logowhite_svgSize;
    if (power && processing)
    {
        bool dark = FxTheme::getThemeMode() == FxThemeMode::Dark;
        svg = dark ? BinaryData::logored_svg : BinaryData::logoblue_svg;
        size = dark ? BinaryData::logored_svgSize : BinaryData::logoblue_svgSize;
    }
    Image image(Image::ARGB, icon_size, icon_size, true);
    if (auto drawable = Drawable::createFromImageData(svg, (size_t)size))
    {
        Graphics g(image);
        drawable->drawWithin(g, Rectangle<float>(0, 0, (float)icon_size, (float)icon_size), RectanglePlacement::centred, power ? 1.0f : 0.45f);
    }
    return image;
}

void FxSystemTrayView::setStatus(bool power, bool processing)
{
    String param = power ? TRANS(L"on") : TRANS(L"off");
    auto& model = FxModel::getModel();
    String status = String(TRANS("FxSound is %s.")).replace("%s", param);
    String output = TRANS("Output: ") + String(model.getSelectedOutput().deviceFriendlyName.c_str());

    if (status_notifier_item_ != nullptr)
    {
        // Several sizes so the panel can pick a sharp one; ARGB32, big-endian.
        std::vector<FxStatusNotifierItem::Pixmap> pixmaps;
        for (int size : { 16, 22, 24, 32, 48, 64 })
        {
            auto image = makeIcon(power, processing, size);
            FxStatusNotifierItem::Pixmap pixmap;
            pixmap.width = pixmap.height = size;
            pixmap.argb.reserve((size_t)size * size * 4);
            for (int y = 0; y < size; y++)
                for (int x = 0; x < size; x++)
                {
                    auto c = image.getPixelAt(x, y);
                    pixmap.argb.insert(pixmap.argb.end(), { c.getAlpha(), c.getRed(), c.getGreen(), c.getBlue() });
                }
            pixmaps.push_back(std::move(pixmap));
        }
        status_notifier_item_->setIcon(std::move(pixmaps));
        status_notifier_item_->setToolTip("FxSound", (status + "\n" + output).toStdString());
    }
    if (auto* icon = dynamic_cast<FxXEmbedTrayIcon*>(xembed_icon_.get()))
    {
        icon->setImage(makeIcon(power, processing));
    }
    scheduleTrayMenuRefresh();
}
#endif

#if JUCE_WINDOWS
Point<int> FxSystemTrayView::getSystemTrayWindowPosition(int width, int height)
{
    Point<int> pos = { 0, 0 };

    NOTIFYICONIDENTIFIER icon_id = {};
    RECT rect;

    HWND hWnd = (HWND)getWindowHandle();

    icon_id.cbSize = sizeof(NOTIFYICONIDENTIFIER);
    icon_id.hWnd = hWnd;
    icon_id.guidItem = trayIconGuid_;

    if (FAILED(Shell_NotifyIconGetRect(&icon_id, &rect)))
    {
        return pos;
    }

    auto display = Desktop::getInstance().getDisplays().getPrimaryDisplay();
    if (display != nullptr)
    {
        juce::Rectangle<int> prect{ rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top };
        auto lrect = Desktop::getInstance().getDisplays().physicalToLogical(prect);

        auto area = display->userArea;

        if (lrect.getX() < area.getCentreX())
        {
            pos.x = area.getX() + 10;
        }
        else
        {
            pos.x = area.getRight() - width - 10;
        }


        if (lrect.getY() < area.getCentreY())
        {
            pos.y = area.getY() + 10;
        }
        else
        {
            pos.y = area.getBottom() - height - 10;
        }
    }

    return pos;
}
#else
Point<int> FxSystemTrayView::getSystemTrayWindowPosition(int width, int height)
{
    // There is no API for the tray icon's position on Linux. Use the corner
    // nearest to where the icon was last clicked (as Windows uses the icon
    // rectangle); before any click, the top-right corner (GNOME/KDE panels).
    Point<int> pos = { 0, 0 };
    auto display = Desktop::getInstance().getDisplays().getPrimaryDisplay();
    if (display == nullptr)
        return pos;
    auto area = display->userArea;
    auto click = last_click_position_.isOrigin() ? Point<int>(area.getRight(), area.getY()) : last_click_position_;
    pos.x = click.x < area.getCentreX() ? area.getX() + 10 : area.getRight() - width - 10;
    pos.y = click.y < area.getCentreY() ? area.getY() + 10 : area.getBottom() - height - 10;
    return pos;
}
#endif

#if JUCE_WINDOWS
void FxSystemTrayView::addIcon()
{
    NOTIFYICONDATA nid = { sizeof(nid) };

    HINSTANCE hInst = GetModuleHandle(NULL);
    HWND hWnd = (HWND)getWindowHandle();

    if (FxModel::getModel().getPowerState())
    {
        if (FxController::getInstance().isAudioProcessing())
        {
            if (FxTheme::getThemeMode() == FxThemeMode::Dark)
            {
                nid.hIcon = LoadIcon(hInst, L"IDI_LOGO_RED");
            }
            else
            {
                nid.hIcon = LoadIcon(hInst, L"IDI_LOGO_BLUE");
            }
        }
        else
        {
            nid.hIcon = LoadIcon(hInst, L"IDI_LOGO_WHITE");
        }
    }
    else
    {
        nid.hIcon = LoadIcon(hInst, L"IDI_LOGO_GRAY");
    }

    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP | NIF_GUID;
    nid.guidItem = trayIconGuid_;
    nid.uCallbackMessage = WMAPP_FXTRAYICON;
    nid.hWnd = hWnd;
    lstrcpy(nid.szTip, L"FxSound");
    Shell_NotifyIcon(NIM_ADD, &nid);

    // NOTIFYICON_VERSION_4 is prefered
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIcon(NIM_SETVERSION, &nid);

    setVisible(true);
}
#else
void FxSystemTrayView::addIcon()
{
    setStatus(FxModel::getModel().getPowerState(), FxController::getInstance().isAudioProcessing());
}
#endif

void FxSystemTrayView::showContextMenu()
{
    FxController::getInstance().checkDeviceChanges();
    auto context_menu = createContextMenu();

#if JUCE_WINDOWS
    HWND hWnd = (HWND)getWindowHandle();

    SetFocus(hWnd);
    SetForegroundWindow(hWnd);

#endif
    context_menu.show();
}

PopupMenu FxSystemTrayView::createContextMenu()
{
    PopupMenu context_menu;

    PopupMenu preset_menu;
    PopupMenu theme_menu;

    auto id = PRESET_MENU_ID_START;
    auto count = FxModel::getModel().getPresetCount();
    auto preset_type = FxModel::PresetType::AppPreset;
    for (auto i = 0; i < count; i++)
    {
        auto preset = FxModel::getModel().getPreset(i);
        PopupMenu::Item menu_item(preset.modified ? preset.name + L" *" : preset.name);
        menu_item.setID(id);
        if (id - PRESET_MENU_ID_START == FxModel::getModel().getSelectedPreset())
        {
            menu_item.setTicked(true);
        }

        if (preset_type != preset.type)
        {
            preset_menu.addSeparator();
            preset_type = preset.type;
        }

        auto handler = [preset = id]() {
            FxController::getInstance().setPreset(preset - PRESET_MENU_ID_START);
        };
        menu_item.setAction(handler);

        preset_menu.addItem(menu_item);
        id++;
    }
   
    auto openClicked = []() {
        FxController::getInstance().showMainWindow();
    };

    auto powerClicked = []() {
        FxController::getInstance().setPowerState(!FxModel::getModel().getPowerState());
    };

    auto settingsClicked = []() {
        FxSettingsDialog settings_dialog;
        settings_dialog.runModalLoop();
        FxController::getInstance().refreshOutputList();
    };

    auto darkModeClicked = [this]() {
        FxController::getInstance().setThemeMode(FxThemeMode::Dark);
        };

    auto lightModeClicked = [this]() {
        FxController::getInstance().setThemeMode(FxThemeMode::Light);
        };

    auto alwaysOnTopClicked = []() {
        auto& controller = FxController::getInstance();
        controller.setAlwaysOnTop(!controller.isAlwaysOnTop());
    };

    auto donateClicked = []() {
        URL url("https://www.paypal.com/donate/?hosted_button_id=JVNQGYXCQ2GPG");
        url.launchInDefaultBrowser();
    };

    auto exitClicked = []() {
        FxController::getInstance().exit();
    };

    theme_menu.addItem(TRANS("Dark"), true, FxTheme::getThemeMode() == FxThemeMode::Dark, darkModeClicked);
    theme_menu.addItem(TRANS("Light"), true, FxTheme::getThemeMode() == FxThemeMode::Light, lightModeClicked);

    PopupMenu::Item open(TRANS("Open"));
    PopupMenu::Item power;
    PopupMenu::Item settings(TRANS("Settings"));
    PopupMenu::Item donate(TRANS("Donate"));
    PopupMenu::Item exit(TRANS("Exit"));

    open.setID(MENU_ID_OPEN);
    open.setAction(openClicked);
    power.text = FxModel::getModel().getPowerState() ? String(TRANS("Turn Off")) : String(TRANS("Turn On"));    
    power.setID(MENU_ID_POWER);
    power.setAction(powerClicked);
    power.setEnabled(!SysInfo::isRemoteSession());
    settings.setID(MENU_ID_SETTINGS);
    settings.setAction(settingsClicked);
    donate.setID(MENU_ID_DONATE);
    donate.setAction(donateClicked);
    exit.setID(MENU_ID_EXIT);
    exit.setAction(exitClicked);

    context_menu.addItem(open);
    context_menu.addItem(power);
    if (FxModel::getModel().getPowerState())
    {
        context_menu.addSubMenu(TRANS("Preset Select"), preset_menu);
    }
    addOutputDeviceMenu(&context_menu);
    context_menu.addItem(settings);
    context_menu.addSubMenu(TRANS("Theme"), theme_menu);
    context_menu.addItem(TRANS("Always On Top"), true, FxController::getInstance().getMainWindow()->isAlwaysOnTop(), alwaysOnTopClicked);
    context_menu.addItem(donate);
    context_menu.addItem(exit);

    return context_menu;
}

void FxSystemTrayView::addOutputDeviceMenu(PopupMenu* context_menu)
{
    PopupMenu output_menu;
    PopupMenu* menu;

    auto output_devices = FxModel::getModel().getOutputDevices();
    int num_outputs = output_devices.size();
    if (num_outputs > 5)
    {
        menu = &output_menu;
    }
    else
    {
        menu = context_menu;
        menu->addSeparator();
        menu->addSectionHeader(TRANS("Playback Device Select"));
    }

    auto id = OUTPUT_MENU_ID_START;
    for (auto device : output_devices)
    {
        PopupMenu::Item menu_item(getTruncatedText(device.deviceFriendlyName.c_str(), 30));
        menu_item.setID(id);
        if (device.deviceNumChannel < 2)
        {
            menu_item.setEnabled(false);
        }

        if (id - OUTPUT_MENU_ID_START == FxModel::getModel().getSelectedOutputIndex())
        {
            menu_item.setTicked(true);
        }

        auto handler = [output = id]() {
            FxController::getInstance().setOutput(output - OUTPUT_MENU_ID_START);
            };
        menu_item.setAction(handler);

        menu->addItem(menu_item);
        id++;
    }

    if (num_outputs > 5)
    {
        context_menu->addSubMenu(TRANS("Playback Device Select"), output_menu);
    }
    else
    {
        menu->addSeparator();
    }
}

void FxSystemTrayView::showNotification()
{
    String message;
    std::pair<String, String> link;
    FxModel::getModel().popMessage(message, link);

    if (message.isNotEmpty())
    {
#if JUCE_WINDOWS
        if (custom_notification_ || link.first.isNotEmpty())
#else
        if (true)
#endif
        {
#if JUCE_WINDOWS
            QUERY_USER_NOTIFICATION_STATE quns;
            if (FAILED(SHQueryUserNotificationState(&quns)) || quns != QUNS_ACCEPTS_NOTIFICATIONS)
            {
                return;
            }

#endif
            notification_.setMessage(message, link);
            Point<int> pos = getSystemTrayWindowPosition(notification_.getWidth(), notification_.getHeight());
            notification_.setBounds(pos.x, pos.y, notification_.getWidth(), notification_.getHeight());
            notification_.showMessage();
        }
#if JUCE_WINDOWS
        else
        {
            NOTIFYICONDATA nid = { sizeof(nid) };

            nid.uFlags = NIF_INFO | NIF_GUID | NIF_REALTIME;
            nid.guidItem = trayIconGuid_;
            nid.dwInfoFlags = NIIF_NOSOUND | NIIF_RESPECT_QUIET_TIME;

            String title = L"FxSound";
            title.copyToUTF16(nid.szInfoTitle, sizeof(nid.szInfoTitle) - 1);
            message.copyToUTF16(nid.szInfo, sizeof(nid.szInfo) - 1);

            Shell_NotifyIcon(NIM_MODIFY, &nid);
        }
#endif
    }
}

String FxSystemTrayView::getTruncatedText(const String& text, int max_length)
{
    if (text.length() <= max_length)
        return text;
    else
    {
        const String ellipsis = "...";
        int trunc_length = (text.length() - max_length) + ellipsis.length();
        return text.dropLastCharacters(trunc_length) + ellipsis;
    }
}

#if JUCE_WINDOWS
LRESULT CALLBACK FxSystemTrayView::wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto tray_view = reinterpret_cast<FxSystemTrayView*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    if (message == tray_view->taskbar_created_message_)
    {
        tray_view->addIcon();
    }
    else if (message == WMAPP_FXTRAYICON)
    {
        switch (LOWORD(lParam))
        {
        case NIN_SELECT:
            if (FxController::getInstance().isMainWindowVisible())
            {
                FxController::getInstance().hideMainWindow();
            }
            else
            {
                FxController::getInstance().showMainWindow();
            }
            break;

        case NIN_BALLOONTIMEOUT:
        case NIN_BALLOONUSERCLICK:
        {
            NOTIFYICONDATA nid = { sizeof(nid) };
            nid.uFlags = NIF_SHOWTIP | NIF_GUID;
            nid.guidItem = trayIconGuid_;
            Shell_NotifyIcon(NIM_MODIFY, &nid);
        }
        break;

        case WM_CONTEXTMENU:
            tray_view->showContextMenu();
            break;
        }
    }
    else
    {
        return CallWindowProc(tray_view->componentWndProc_, hwnd, message, wParam, lParam);
    }

    return 0;
}
#else
void FxSystemTrayView::onTrayClick(Point<int> screen_position, bool popup_menu)
{
    // Same actions as Windows: left click toggles the window, right click opens
    // the menu (a StatusNotifierItem host shows the D-Bus menu itself).
    if (!screen_position.isOrigin())
    {
        last_click_position_ = screen_position;
    }
    if (popup_menu)
    {
        showContextMenu();
    }
    else if (FxController::getInstance().isMainWindowVisible())
    {
        FxController::getInstance().hideMainWindow();
    }
    else
    {
        FxController::getInstance().showMainWindow();
    }
}

void FxSystemTrayView::scheduleTrayMenuRefresh()
{
    if (status_notifier_item_ == nullptr || tray_menu_refresh_pending_)
        return;
    tray_menu_refresh_pending_ = true;
    SafePointer<FxSystemTrayView> safe(this);
    MessageManager::callAsync([safe] { if (safe != nullptr) safe->refreshTrayMenu(false); });
}

void FxSystemTrayView::refreshTrayMenu(bool check_devices)
{
    tray_menu_refresh_pending_ = false;
    if (status_notifier_item_ == nullptr)
        return;
    if (check_devices)
    {
        FxController::getInstance().checkDeviceChanges();   // as when the Windows menu opens
    }
    auto menu = createContextMenu();
    tray_menu_generation_ = (tray_menu_generation_ + 1) % 100000;
    tray_menu_actions_.clear();
    status_notifier_item_->setMenu(toTrayMenuItems(menu));
}

std::vector<FxStatusNotifierItem::MenuItem> FxSystemTrayView::toTrayMenuItems(const PopupMenu& menu)
{
    std::vector<FxStatusNotifierItem::MenuItem> items;
    for (PopupMenu::MenuItemIterator it(menu); it.next();)
    {
        auto& item = it.getItem();
        FxStatusNotifierItem::MenuItem out;
        if (item.isSeparator)
        {
            // No separators at the start or end, or two in a row.
            if (items.empty() || items.back().separator)
                continue;
            out.separator = true;
        }
        else
        {
            out.label = item.text.toStdString();
            out.enabled = item.isEnabled && !item.isSectionHeader;
            out.checked = item.isTicked;
            if (item.subMenu != nullptr)
            {
                out.children = toTrayMenuItems(*item.subMenu);
            }
            else if (out.enabled && item.action != nullptr)
            {
                out.action = tray_menu_generation_ * TRAY_ACTIONS_PER_MENU + (int)tray_menu_actions_.size();
                tray_menu_actions_.push_back(item.action);
            }
        }
        items.push_back(std::move(out));
    }
    while (!items.empty() && items.back().separator)
        items.pop_back();
    return items;
}

void FxSystemTrayView::runTrayMenuAction(int action)
{
    // Ignore clicks on a menu that has been rebuilt since it was shown.
    if (action / TRAY_ACTIONS_PER_MENU != tray_menu_generation_)
        return;
    auto index = (size_t)(action % TRAY_ACTIONS_PER_MENU);
    if (index >= tray_menu_actions_.size())
        return;
    auto handler = tray_menu_actions_[index];   // copy: the action may rebuild the menu (e.g. Settings runs a modal loop)
    handler();
}
#endif