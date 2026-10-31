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
#if JUCE_WINDOWS
#include <comdef.h>
#endif
#include "GUI/FxSystemTrayView.h"
#include "GUI/FxController.h"
#include "GUI/FxTheme.h"
#include "GUI/FxMainWindow.h"
#include "AudioPassthru.h"
#if JUCE_WINDOWS
#include <dbghelp.h>

#pragma comment(lib, "dbghelp.lib")
#endif

#if JUCE_WINDOWS
// Do not use high-performance GPU on laptops with dual graphics adapters
extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000000;

    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 0;
}
#endif

#if JUCE_LINUX
#include "FxInstanceChannel.h"   // linux/gui: single-instance command forwarding
#include <csignal>

// Desktop logout and `systemctl --user stop` end apps with SIGTERM. Route it
// (and SIGINT/SIGHUP) through the normal quit path so shutdown() runs, which
// auto-saves a modified preset. The handler only sets a flag; a timer on the
// message thread acts on it, since little else is async-signal-safe.
static std::atomic<bool> g_quit_signal { false };
static void onQuitSignal(int) { g_quit_signal = true; }

class FxQuitSignalWatcher : private Timer
{
public:
    FxQuitSignalWatcher()
    {
        struct sigaction sa {};
        sa.sa_handler = onQuitSignal;
        sigemptyset(&sa.sa_mask);
        for (int sig : { SIGTERM, SIGINT, SIGHUP })
            sigaction(sig, &sa, nullptr);
        startTimer(200);
    }
private:
    void timerCallback() override
    {
        if (g_quit_signal.exchange(false))
            if (auto* app = JUCEApplication::getInstance())
                app->systemRequestedQuit();
    }
};
#endif

//==============================================================================
class FxSoundApplication : public JUCEApplication
{
public:
    //==============================================================================
    FxSoundApplication() { }

    const String getApplicationName() override       { return ProjectInfo::projectName; }
    const String getApplicationVersion() override    { return ProjectInfo::versionString; }
#if JUCE_LINUX
    // Handled by FxInstanceChannel instead (see above).
    bool moreThanOneInstanceAllowed() override       { return true; }
#else
    bool moreThanOneInstanceAllowed() override       { return false; }
#endif

    //==============================================================================
    void initialise (const String& commandline) override
    {
#if JUCE_LINUX
        // Become the running instance, or hand our command line to it. If an
        // instance holds the lock but can't be reached, it is shutting down:
        // keep trying for a few seconds and take over when it has gone.
        for (int attempt = 0; !instance_lock_.enter(0); ++attempt)
        {
            String reply;
            if (FxInstanceChannel::forward(commandline, reply))
            {
                if (reply.isNotEmpty())
                    std::cout << reply << std::endl;
                quit();
                return;
            }
            if (attempt >= 25)   // ~5 s
            {
                std::cerr << "FxSound is already running but not responding." << std::endl;
                quit();
                return;
            }
            Thread::sleep(200);
        }
#endif
        // This method is where you should put your application's initialisation code..
        try
        {
#if JUCE_WINDOWS
            SetUnhandledExceptionFilter(unhandledExceptionFilter);

            HRESULT hRes = CoInitializeEx(0, COINIT_MULTITHREADED);
            if (SUCCEEDED(hRes))
            {
                CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
                    RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
            }
#endif
            
            LookAndFeel::setDefaultLookAndFeel(&theme_);

            setWorkingDirectory();

            FxController::getInstance().initConfig(commandline);

            audio_passthru_ = std::make_unique<AudioPassthru>();
            main_window_ = std::make_unique<FxMainWindow>();
            system_tray_view_.reset(new FxSystemTrayView());

            FxController::getInstance().init(main_window_.get(), system_tray_view_.get(), audio_passthru_.get());
#if JUCE_LINUX
            quit_signal_watcher_ = std::make_unique<FxQuitSignalWatcher>();
            instance_channel_ = std::make_unique<FxInstanceChannel>([this](const String& cmd) -> std::optional<String> {
                if (g_quit_signal)
                    return std::nullopt;   // quitting: let the new instance take over
                String output;
                auto& controller = FxController::getInstance();
                controller.status_output_ = [&output](const String& text) { output = text; };
                anotherInstanceStarted(cmd);
                controller.status_output_ = nullptr;
                return output;
            });
#endif
        }
        catch (const std::exception& e)
        {
            auto& controller = FxController::getInstance();
            controller.logMessage(String::formatted("std::exception: %s\n", e.what()));

            CaptureAndLogCallStack();

            quit();
        }
        catch (...)
        {
            auto& controller = FxController::getInstance();
            controller.logMessage("Unknown exception\n");

            CaptureAndLogCallStack();

            quit();
        }
    }

    void shutdown() override
    {
#if JUCE_LINUX
        instance_channel_.reset();
        quit_signal_watcher_.reset();
#endif
        if (main_window_.get() != nullptr)
        {
            FxController::getInstance().autoSaveModifiedPreset();
            FxController::getInstance().stopTimer();

            audio_passthru_.reset();

            // UIA holds COM references to AccessibilityNativeHandle objects for
            // desktop-resident components. Call UiaDisconnectAllProviders to
            // synchronously release those references before the components are
            // destroyed, so their COM refcounts reach zero in the destructor
            // chain rather than after static destructors have already run.
#if JUCE_WINDOWS
            if (HMODULE hUia = GetModuleHandleW(L"UIAutomationCore.dll"))
            {
                typedef HRESULT (WINAPI* UiaDisconnectAllProvidersFunc)();
                if (auto fn = (UiaDisconnectAllProvidersFunc)GetProcAddress(hUia, "UiaDisconnectAllProviders"))
                    fn();
            }
#endif

            system_tray_view_.reset();

            main_window_.reset(); // (deletes our window)
        }

        LocalisedStrings::setCurrentMappings(nullptr);
        LookAndFeel::setDefaultLookAndFeel(nullptr);

#if JUCE_WINDOWS
        CoUninitialize();
#endif
    }

    //==============================================================================
    void systemRequestedQuit() override
    {
        // This is called when the app is being asked to quit: you can ignore this
        // request and let the app carry on running, or call quit() to allow the app to close.
        
        quit();
    }

    void anotherInstanceStarted (const String& commandline) override
    {
        FxController::getInstance().applyConfig(commandline);
#if JUCE_LINUX
        // With no tray icon (e.g. stock GNOME), launching FxSound again is how
        // the user gets the window back.
        if (commandline.trim().isEmpty())
            FxController::getInstance().showMainWindow();
#endif
    }

private:
    FxTheme theme_;
    static constexpr int MAX_FRAMES = 64;

#if JUCE_WINDOWS
    static LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* exception_info)
    {
        auto& controller = FxController::getInstance();
     
        auto logPath = File::getSpecialLocation(File::userApplicationDataDirectory).getChildFile("FxSound");
        if (!logPath.exists()) {
            logPath.createDirectory();
        }

        juce::File dumpFile = logPath.getChildFile("fxsound.dmp");
        HANDLE hFile = CreateFileW(dumpFile.getFullPathName().toWideCharPointer(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL, nullptr);

        if (hFile != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION dumpInfo;
            dumpInfo.ThreadId = GetCurrentThreadId();
            dumpInfo.ExceptionPointers = exception_info;
            dumpInfo.ClientPointers = FALSE;

            MiniDumpWriteDump(GetCurrentProcess(),
                GetCurrentProcessId(),
                hFile,
                MiniDumpNormal,
                &dumpInfo,
                nullptr,
                nullptr);

            CloseHandle(hFile);
        }

        String message = String::formatted(
            "Unhandled exception\nException code: 0x%X\nException flags: 0x%X\nException address: 0x%p\n",
            exception_info->ExceptionRecord->ExceptionCode,
            exception_info->ExceptionRecord->ExceptionFlags,
            exception_info->ExceptionRecord->ExceptionAddress
        );

        controller.logMessage("\n" + message);

        // Capture stack based on crash context
        CaptureAndLogCallStack(exception_info->ContextRecord);

        return EXCEPTION_EXECUTE_HANDLER;
    }

    static void CaptureAndLogCallStack(CONTEXT* context = nullptr)
    {
        auto& controller = FxController::getInstance();
        String stacktrace_info;

        HANDLE process = GetCurrentProcess();
        HANDLE thread = GetCurrentThread();

        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        SymInitialize(process, NULL, TRUE);

        if (context == nullptr)
        {
            // No crash context - capture live callstack
            void* frames[MAX_FRAMES];
            USHORT capturedFrames = CaptureStackBackTrace(0, MAX_FRAMES, frames, NULL);

            char buffer[sizeof(SYMBOL_INFO) + 256] = { 0 };
            SYMBOL_INFO* symbol = (SYMBOL_INFO*)buffer;
            symbol->MaxNameLen = 255;
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

            for (USHORT i = 0; i < capturedFrames; i++)
            {
                if (SymFromAddr(process, (DWORD64)frames[i], 0, symbol))
                {
                    stacktrace_info += String::formatted("%S at 0x%llX\n", symbol->Name, symbol->Address);

                    DWORD displacement;
                    IMAGEHLP_LINE64 line = { sizeof(IMAGEHLP_LINE64) };
                    if (SymGetLineFromAddr64(process, (DWORD64)frames[i], &displacement, &line))
                    {
                        char* file_name = std::strrchr(line.FileName, '\\');
                        file_name = file_name ? (file_name + 1) : line.FileName;
                        stacktrace_info += String::formatted("    File: %S, Line: %d\n", file_name, line.LineNumber);
                    }
                }
                else
                {
                    stacktrace_info += String::formatted("Unknown function at 0x%p\n", frames[i]);
                }
            }
        }
        else
        {
            // Crash context - walk using StackWalk64
            STACKFRAME64 stackFrame = { 0 };
#if defined(_M_IX86)
            DWORD machineType = IMAGE_FILE_MACHINE_I386;
            stackFrame.AddrPC.Offset = context->Eip;
            stackFrame.AddrPC.Mode = AddrModeFlat;
            stackFrame.AddrFrame.Offset = context->Ebp;
            stackFrame.AddrFrame.Mode = AddrModeFlat;
            stackFrame.AddrStack.Offset = context->Esp;
            stackFrame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_X64)
            DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
            stackFrame.AddrPC.Offset = context->Rip;
            stackFrame.AddrPC.Mode = AddrModeFlat;
            stackFrame.AddrFrame.Offset = context->Rsp;
            stackFrame.AddrFrame.Mode = AddrModeFlat;
            stackFrame.AddrStack.Offset = context->Rsp;
            stackFrame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_ARM64)
            DWORD machineType = IMAGE_FILE_MACHINE_ARM64;
            stackFrame.AddrPC.Offset = context->Pc;
            stackFrame.AddrPC.Mode = AddrModeFlat;
            stackFrame.AddrFrame.Offset = context->Fp;
            stackFrame.AddrFrame.Mode = AddrModeFlat;
            stackFrame.AddrStack.Offset = context->Sp;
            stackFrame.AddrStack.Mode = AddrModeFlat;
#else
#error Unsupported platform
#endif

            char buffer[sizeof(SYMBOL_INFO) + 256] = { 0 };
            SYMBOL_INFO* symbol = (SYMBOL_INFO*)buffer;
            symbol->MaxNameLen = 255;
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

            for (int frame = 0; frame < MAX_FRAMES; ++frame)
            {
                if (!StackWalk64(machineType, process, thread, &stackFrame, context, NULL,
                    SymFunctionTableAccess64, SymGetModuleBase64, NULL))
                {
                    break;
                }

                if (stackFrame.AddrPC.Offset == 0)
                    break;

                DWORD64 address = stackFrame.AddrPC.Offset;

                if (SymFromAddr(process, address, 0, symbol))
                {
                    stacktrace_info += String::formatted("%S at 0x%llX\n", symbol->Name, symbol->Address);

                    DWORD displacement;
                    IMAGEHLP_LINE64 line = { sizeof(IMAGEHLP_LINE64) };
                    if (SymGetLineFromAddr64(process, address, &displacement, &line))
                    {
                        char* file_name = std::strrchr(line.FileName, '\\');
                        file_name = file_name ? (file_name + 1) : line.FileName;
                        stacktrace_info += String::formatted("    File: %S, Line: %d\n", file_name, line.LineNumber);
                    }
                }
                else
                {
                    stacktrace_info += String::formatted("Unknown function at 0x%llX\n", address);
                }
            }
        }

        SymCleanup(process);

        controller.logMessage(stacktrace_info);
    }
#else
    static void CaptureAndLogCallStack()
    {
        FxController::getInstance().logMessage(SystemStats::getStackBacktrace());
    }
#endif

#if JUCE_WINDOWS
    void setWorkingDirectory()
    {
        wchar_t module_file[MAX_PATH];
        DWORD length = GetModuleFileName(NULL, module_file, MAX_PATH);
        if (length > 0)
        {
            std::wstring path(module_file);
            size_t pos = path.find_last_of(L"\\/");
            if (pos != std::string::npos)
            {
                SetCurrentDirectory(path.substr(0, pos).c_str());
            }
        }
    };
#else
    void setWorkingDirectory()
    {
        // Same as Windows: run from the executable's folder so the bundled presets are found.
        File::getSpecialLocation(File::currentExecutableFile).getParentDirectory().setAsCurrentWorkingDirectory();
    }
#endif

    std::unique_ptr<FxMainWindow> main_window_;
    
    std::unique_ptr<FxSystemTrayView> system_tray_view_;
    std::unique_ptr<AudioPassthru> audio_passthru_;
#if JUCE_LINUX
    InterProcessLock instance_lock_ { "FxSound-instance" };
    std::unique_ptr<FxInstanceChannel> instance_channel_;
    std::unique_ptr<FxQuitSignalWatcher> quit_signal_watcher_;
#endif
};

//==============================================================================
// This macro generates the main() routine that launches the app.
START_JUCE_APPLICATION (FxSoundApplication)
