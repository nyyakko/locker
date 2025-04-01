#include <chrono>
#define NOMINMAX

#include "os/process/ProcessWatcher.hpp"
#include "os/process/ProcessInfo.hpp"
#include "Memoizer.hpp"

#include <cstring>
#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <unordered_map>
#include <ranges>
#include <algorithm>
#include <thread>
#include <mutex>

#include "glad/glad.h"
#include "gl/gl.h"
#include <GLFW/glfw3.h>
#include "imgui/imgui.h"
#include "imgui/imgui_impl_glfw.h"
#include "imgui/imgui_impl_opengl3.h"

using namespace liberror;

Result<void> process_listener_thread(
    IEnumWbemClassObject* processCreationListener,
    IEnumWbemClassObject* processDeletionListener,
    std::unordered_map<std::string, std::string>& protectedPrograms,
    std::mutex& protectedProgramsMutex,
    std::vector<ProcessInfo>& requestedProgramsToBlock,
    std::mutex& requestedProgramsToBlockMutex,
    std::vector<std::string>& currentlyRunningProtectedPrograms,
    std::mutex& currentlyRunningProtectedProgramsMutex,
    std::atomic_bool const& shouldStop)
{
    while (!shouldStop.load())
    {
        // Process Creation Event Listener
        {
            IWbemClassObject* object = nullptr;
            ULONG result = 0;
            processCreationListener->Next(WBEM_NO_WAIT, 1, &object, &result);
            if (result == 0) continue;

            auto processInfo = get_started_process_info(object);
            auto processName = processInfo.name | std::views::transform(tolower) | std::ranges::to<std::string>();

            std::unique_lock protectedProgramsLock { protectedProgramsMutex };
            auto isProgramProtected = protectedPrograms.contains(processName);
            protectedProgramsLock.unlock();

            std::unique_lock requestedProgramsToBlockLock { requestedProgramsToBlockMutex };
            auto isProgramQueuedToBlock = std::ranges::contains(requestedProgramsToBlock, processInfo.name, &ProcessInfo::name);
            requestedProgramsToBlockLock.unlock();

            std::unique_lock currentlyRunningProtectedProgramsLock { currentlyRunningProtectedProgramsMutex };
            auto isProtectedProgramRunning = std::ranges::contains(currentlyRunningProtectedPrograms, processInfo.name);
            currentlyRunningProtectedProgramsLock.unlock();

            if (isProgramProtected && !isProgramQueuedToBlock && !isProtectedProgramRunning)
            {
                std::scoped_lock requestedProgramsToBlockLock_ { requestedProgramsToBlockMutex };
                requestedProgramsToBlock.push_back(processInfo);
            }

            object->Release();
        }
        // Process Deletion Event Listener
        {
            IWbemClassObject* object = nullptr;
            ULONG result = 0;
            processDeletionListener->Next(WBEM_NO_WAIT, 1, &object, &result);
            if (result == 0) continue;

            auto processInfo = get_started_process_info(object);
            std::scoped_lock currentlyRunningProtectedProgramsLock { currentlyRunningProtectedProgramsMutex };
            if (std::ranges::contains(currentlyRunningProtectedPrograms, processInfo.name))
            {
                auto processes = TRY(get_running_processes());
                if (!processes.contains(processInfo.name))
                {
                    currentlyRunningProtectedPrograms.erase(std::ranges::find(currentlyRunningProtectedPrograms, processInfo.name));
                    spdlog::info("[processListenerThread]: protected process {} was closed", processInfo.name);
                }
            }

            object->Release();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    return {};
}

Result<void> process_blocker_thread(
    std::vector<ProcessInfo> const& requestedProgramsToBlock,
    std::mutex& requestedProgramToBlockMutex,
    std::vector<ProcessInfo>& currentlyBlockedProcesses,
    std::mutex& currentlyBlockedProcessesMutex,
    std::atomic_bool const& shouldStop)
{
    while (!shouldStop.load())
    {
        auto processes = TRY(get_running_processes());
        for (auto const& process : processes)
        {
            std::unique_lock requestedProgramsToBlockLock { requestedProgramToBlockMutex };
            auto isProgramQueuedToBlock = std::ranges::contains(requestedProgramsToBlock | std::views::transform(&ProcessInfo::name), process.first);
            requestedProgramsToBlockLock.unlock();

            std::unique_lock currentlyBlockedProcessesLock { currentlyBlockedProcessesMutex };
            auto isProgramAlreadyBlocked = std::ranges::contains(currentlyBlockedProcesses | std::views::transform(&ProcessInfo::name), process.first);
            currentlyBlockedProcessesLock.unlock();

            if (isProgramQueuedToBlock && !isProgramAlreadyBlocked)
            {
                for (auto const& processToBlock : process.second)
                {
                    std::scoped_lock currentlyBlockedProcessesLock_ { currentlyBlockedProcessesMutex };
                    currentlyBlockedProcesses.push_back(processToBlock);
                    TRY(suspend_process_thread(processToBlock));
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    return {};
}

size_t calculate_edit_distance(std::string_view a, std::string_view b)
{
    Memoizer<size_t(std::string_view, std::string_view)> memoizer {};

    memoizer = [&memoizer] (std::string_view a, std::string_view b) {
        if (a.empty()) return b.size();
        if (b.empty()) return a.size();
        auto const tailA = a.substr(1);
        auto const tailB = b.substr(1);
        if (a.front() == b.front()) return memoizer(tailA, tailB);
        return 1 + std::ranges::min({
            memoizer(tailA, b),
            memoizer(a, tailB),
            memoizer(tailA, tailB)
        });
    };

    return memoizer(a, b);
}

int main()
{
    glfwInit();

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_RESIZABLE, false);

    auto window = glfwCreateWindow(825, 600, "locker", nullptr, nullptr);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    int width, height;
    glfwGetFramebufferSize(window, &width, &height);

    MUST(initialize_com());

    IWbemLocator* locator = nullptr;
    IWbemServices* service = nullptr;

    MUST(connect_to_wmi(locator, service));
    MUST(set_wmi_proxy_blanket(locator, service));
    auto processCreationListener = MUST(get_process_creation_event_listener(locator, service));
    auto processDeletionListener = MUST(get_process_deletion_event_listener(locator, service));

    std::atomic_bool shouldStop = false;

    std::unordered_map<std::string, std::string> protectedPrograms {};
    std::mutex protectedProgramsMutex;

    std::vector<ProcessInfo> requestedProgramsToBlock {};
    std::mutex requestedProgramsToBlockMutex;

    std::vector<ProcessInfo> currentlyBlockedProcesses {};
    std::mutex currentlyBlockedProcessesMutex;

    std::vector<std::string> currentlyRunningProtectedPrograms {};
    std::mutex currentlyRunningProtectedProgramsMutex;

    std::thread processListenerThread { [processCreationListener, processDeletionListener, &protectedPrograms, &protectedProgramsMutex, &requestedProgramsToBlock, &requestedProgramsToBlockMutex, &currentlyRunningProtectedPrograms, &currentlyRunningProtectedProgramsMutex, &shouldStop] () {
        auto result = process_listener_thread(processCreationListener, processDeletionListener, protectedPrograms, protectedProgramsMutex, requestedProgramsToBlock, requestedProgramsToBlockMutex, currentlyRunningProtectedPrograms, currentlyRunningProtectedProgramsMutex, shouldStop);
        if (!result.has_value())
            spdlog::critical("[processListenerThread]: failed with error: {}", result.error().message());
    }};

    std::thread processBlockerThread { [&requestedProgramsToBlock, &requestedProgramsToBlockMutex, &currentlyBlockedProcesses, &currentlyBlockedProcessesMutex, &shouldStop] () {
        auto result = process_blocker_thread(requestedProgramsToBlock, requestedProgramsToBlockMutex, currentlyBlockedProcesses, currentlyBlockedProcessesMutex, shouldStop);
        if (!result.has_value())
            spdlog::critical("[processListenerThread]: failed with error: {}", result.error().message());
    }};

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos({});
        ImGui::SetNextWindowSize({ static_cast<float>(width), static_cast<float>(height) });
        ImGui::Begin("Locker", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

        std::unique_lock protectedProgramsLock { protectedProgramsMutex };

        if (!currentlyBlockedProcesses.empty())
        {
            ImGui::OpenPopup("unlock_program_popup");
        }
        protectedProgramsLock.unlock();

        if (ImGui::BeginPopup("unlock_program_popup"))
        {
            static char password[256] = {};
            ImGui::Text("Password");
            ImGui::InputText("##password", password, sizeof(password));
            if (ImGui::Button("Unlock"))
            {
                protectedProgramsLock.lock();
                std::scoped_lock currentlyBlockedProcessesLock { currentlyBlockedProcessesMutex };
                auto blockedProgramName = currentlyBlockedProcesses.back().name | std::views::transform(tolower) | std::ranges::to<std::string>();
                if (protectedPrograms.contains(blockedProgramName) && protectedPrograms.at(blockedProgramName) == password)
                {
                    for (auto const& process : currentlyBlockedProcesses)
                    {
                        auto processName = process.name | std::views::transform(tolower) | std::ranges::to<std::string>();
                        if (protectedPrograms.at(processName) == password)
                            MUST(resume_process_thread(process));
                    }

                    std::scoped_lock currentlyRunningProtectedProgramsLock { currentlyRunningProtectedProgramsMutex };
                    currentlyRunningProtectedPrograms.push_back(currentlyBlockedProcesses.back().name);

                    std::scoped_lock requestedProgramsToBlockLock { requestedProgramsToBlockMutex };
                    if (!requestedProgramsToBlock.empty())
                        requestedProgramsToBlock.erase(std::ranges::find(requestedProgramsToBlock, currentlyBlockedProcesses.back()));

                    spdlog::info("[mainThread]: successfully unlocked process {}", currentlyBlockedProcesses.back().name);

                    currentlyBlockedProcesses.clear();
                    auto [first, last] = std::ranges::remove_if(currentlyBlockedProcesses, [&protectedPrograms] (ProcessInfo const& process) {
                        return protectedPrograms.at(process.name) == password;
                    });
                    currentlyBlockedProcesses.erase(first, last);

                    ImGui::CloseCurrentPopup();
                }
                protectedProgramsLock.unlock();
            }
            ImGui::EndPopup();
        }

        auto processes = MUST(get_running_processes());

        ImGui::BeginGroup();
            static char searchProcessName[MAX_PATH] = {};
            static std::string previousSearchProcessName {};
            ImGui::Text("Search Process");
            ImGui::SetNextItemWidth(400);
            ImGui::InputText("##search_process", searchProcessName, sizeof(searchProcessName));

            auto searchProcessNameFixed = std::string_view(searchProcessName) | std::views::transform(tolower) | std::ranges::to<std::string>();
            auto filteredProcesses = std::views::filter(processes, [&searchProcessNameFixed] (std::pair<std::string, std::vector<ProcessInfo>> const& lhs) {
                if (searchProcessNameFixed.empty()) return true;
                auto lhsFixed = lhs.first | std::views::transform(tolower) | std::ranges::to<std::string>();
                lhsFixed = lhsFixed.substr(0, lhsFixed.find("."));
                auto distanceLhs = static_cast<float>(calculate_edit_distance(searchProcessNameFixed, lhsFixed));
                auto sizeLhs = static_cast<float>(std::ranges::max(lhsFixed.size(), searchProcessNameFixed.size()));
                return (sizeLhs - distanceLhs) / sizeLhs * 100.f > 50;
            });

            ImGui::Text("Running Processes");
            if (ImGui::BeginTable("##running_processes", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(400, 200)))
            {
                ImGui::TableSetupColumn("Process Name");
                ImGui::TableSetupColumn("Process Id");
                ImGui::TableHeadersRow();

                static auto selectedRow = -1;
                static std::string selectedProcessName {};

                for (auto const& [rowIndex, process] : filteredProcesses | std::views::enumerate)
                {
                    bool selected = static_cast<int>(rowIndex) == selectedRow;

                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    ImGui::Text("%s", process.first.data());
                    ImGui::TableNextColumn();
                    ImGui::Text("%lu", process.second.front().pid);

                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(fmt::format("##{}", rowIndex).data(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        selectedRow = static_cast<int>(rowIndex);
                        selectedProcessName = process.first | std::views::transform(tolower) | std::ranges::to<std::string>();
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        {
                            ImGui::OpenPopup("protect_program_popup");
                        }
                    }
                }

                if (ImGui::BeginPopup("protect_program_popup"))
                {
                    static char password[256] = {};
                    ImGui::Text("Password");
                    ImGui::InputText("##password", password, sizeof(password));
                    if (ImGui::Button("Protect"))
                    {
                        protectedProgramsLock.lock();
                        protectedPrograms.insert({ selectedProcessName, password });
                        protectedProgramsLock.unlock();
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }

                ImGui::EndTable();
            }

        ImGui::EndGroup();

        ImGui::SameLine();

        ImGui::BeginGroup();
            static char searchProgramName[MAX_PATH] = {};
            ImGui::Text("Search Program");
            ImGui::SetNextItemWidth(400);
            ImGui::InputText("##search_program", searchProgramName, sizeof(searchProcessName));
            ImGui::Text("Protected Programs");
            if (ImGui::BeginTable("##protected_programs", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(400, 200)))
            {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Password");
                ImGui::TableHeadersRow();

                static auto selectedRow = -1;

                protectedProgramsLock.lock();
                for (auto const& [rowIndex, value] : std::views::enumerate(protectedPrograms))
                {
                    auto const& program = value;
                    bool selected = static_cast<int>(rowIndex) == selectedRow;

                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    ImGui::Text("%s", program.first.data());
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", program.second.data());

                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(fmt::format("##{}", rowIndex).data(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        selectedRow = static_cast<int>(rowIndex);
                    }
                }
                protectedProgramsLock.unlock();

                ImGui::EndTable();
            }
        ImGui::EndGroup();

        ImGui::End();

        ImGui::Render();
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.f, 0.f, 0.f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    shouldStop.store(true);

    processBlockerThread.join();
    processListenerThread.join();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    service->Release();
    locator->Release();
    processCreationListener->Release();
    processDeletionListener->Release();
    CoUninitialize();
}
