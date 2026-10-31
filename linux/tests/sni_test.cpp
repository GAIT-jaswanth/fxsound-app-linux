// Drives FxStatusNotifierItem for tests/sni_test.py (needs a D-Bus session).
// Prints events on stdout; reads commands on stdin: "menu <n>", "icon", "quit".
#include "FxStatusNotifierItem.h"
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>

static std::mutex out_mutex;
static void say(const std::string& s)
{
    std::lock_guard<std::mutex> lock(out_mutex);
    std::cout << s << std::endl;
}

static std::vector<FxStatusNotifierItem::MenuItem> makeMenu(int extra)
{
    using M = FxStatusNotifierItem::MenuItem;
    std::vector<M> menu;
    M open; open.label = "Open"; open.action = 1; menu.push_back(open);
    M sep; sep.separator = true; menu.push_back(sep);
    M presets; presets.label = "Preset_Select";
    for (int i = 0; i < 3 + extra; i++)
    {
        M p; p.label = "Preset " + std::to_string(i); p.action = 100 + i; p.checked = (i == 1);
        presets.children.push_back(p);
    }
    menu.push_back(presets);
    M header; header.label = "Playback Device"; header.enabled = false; menu.push_back(header);
    M exit; exit.label = "Exit"; exit.action = 5; menu.push_back(exit);
    return menu;
}

static std::vector<FxStatusNotifierItem::Pixmap> makeIcon(uint8_t red)
{
    std::vector<FxStatusNotifierItem::Pixmap> icons;
    for (int size : { 16, 22 })
    {
        FxStatusNotifierItem::Pixmap p;
        p.width = p.height = size;
        for (int i = 0; i < size * size; i++)
            p.argb.insert(p.argb.end(), { 255, red, 0, 0 });
        icons.push_back(p);
    }
    return icons;
}

int main()
{
    FxStatusNotifierItem* item_ptr = nullptr;
    int about_count = 0;
    FxStatusNotifierItem::Callbacks cb;
    cb.activate = [](int x, int y) { say("ACTIVATE " + std::to_string(x) + " " + std::to_string(y)); };
    cb.menuAction = [](int a) { say("ACTION " + std::to_string(a)); };
    cb.menuAboutToShow = [&] { say("ABOUT"); if (item_ptr) item_ptr->setMenu(makeMenu(++about_count)); };
    cb.hostChanged = [](bool on) { say(std::string("HOST ") + (on ? "1" : "0")); };
    {
        FxStatusNotifierItem item("fxsound", "FxSound", cb);
        item_ptr = &item;
        item.setIcon(makeIcon(200));
        item.setToolTip("FxSound", "FxSound is on.\nOutput: Speakers_1");
        item.setMenu(makeMenu(0));
        say(std::string("REGISTERED ") + (item.waitForHost(2000) ? "1" : "0"));

        std::string line;
        while (std::getline(std::cin, line))
        {
            if (line == "quit")
                break;
            if (line.rfind("menu ", 0) == 0)
                item.setMenu(makeMenu(std::stoi(line.substr(5))));
            if (line == "icon")
                item.setIcon(makeIcon(10));
            say("OK " + line);
        }
        item_ptr = nullptr;
    }
    say("EXITED");
    return 0;
}
