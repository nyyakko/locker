#include <functional>
#define NOMINMAX

#include "os/process/ProcessWatcher.hpp"
#include "os/process/ProcessInfo.hpp"
#include "Memoizer.hpp"

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <cstring>
#include <unordered_map>
#include <ranges>
#include <algorithm>

#include "glad/glad.h"
#include "gl/gl.h"
#include <GLFW/glfw3.h>
#include "imgui/imgui.h"
#include "imgui/imgui_impl_glfw.h"
#include "imgui/imgui_impl_opengl3.h"

using namespace liberror;

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

struct ProcessListenerContext
{
    IEnumWbemClassObject* processCreationListener;
    IEnumWbemClassObject* processDeletionListener;
    std::unordered_map<std::string, std::string>& protectedPrograms;
    std::unordered_map<std::basic_string<char>, std::vector<ProcessInfo>>& runningProcesses;
    std::unordered_map<std::basic_string<char>, std::vector<ProcessInfo>>& suspensionQueue;
    std::unordered_map<std::basic_string<char>, std::vector<ProcessInfo>>& suspendedProcesses;
    std::unordered_map<std::basic_string<char>, std::vector<ProcessInfo>>& resumedProcesses;
};

void process_creation_handler(ProcessListenerContext& ctx)
{
    IWbemClassObject* object = nullptr;
    ULONG result = 0;
    ctx.processCreationListener->Next(WBEM_NO_WAIT, 1, &object, &result);
    if (result == 0) return;

    auto process = get_started_process_info(object);

    if (ctx.protectedPrograms.contains(process.name) && !ctx.resumedProcesses.contains(process.name))
    {
        ctx.suspensionQueue[process.name].push_back(process);
    }
}

void process_deletion_handler(ProcessListenerContext& ctx)
{
    IWbemClassObject* object = nullptr;
    ULONG result = 0;
    ctx.processDeletionListener->Next(WBEM_NO_WAIT, 1, &object, &result);
    if (result == 0) return;

    auto process = get_started_process_info(object);

    if (!ctx.runningProcesses.contains(process.name) && ctx.resumedProcesses.contains(process.name))
    {
        ctx.resumedProcesses.erase(process.name);
    }
}

void process_suspension_handler(ProcessListenerContext& ctx)
{
    for (auto& process : ctx.suspensionQueue)
    {
        for (auto& processThread : process.second)
        {
            ctx.suspendedProcesses[process.first].push_back(processThread);
            MUST(suspend_process_thread(processThread));
        }
    }

    std::ranges::for_each(ctx.suspendedProcesses, [&] (auto&& suspendedProcess) {
        ctx.suspensionQueue.erase(suspendedProcess.first);
    });
}

void process_resumption_handler(ProcessListenerContext& ctx, std::string_view password)
{
    for (auto& process : ctx.suspendedProcesses)
    {
        if (ctx.protectedPrograms.at(process.first) != password) continue;

        for (auto& processThread : process.second)
        {
            ctx.resumedProcesses[process.first].push_back(processThread);
            MUST(resume_process_thread(processThread));
        }
    }

    std::ranges::for_each(ctx.resumedProcesses, [&] (auto&& resumedProcess) {
        ctx.suspendedProcesses.erase(resumedProcess.first);
    });
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

    std::unordered_map<std::string, std::string> protectedProcesses {};
    std::unordered_map<std::basic_string<char>, std::vector<ProcessInfo>> suspensionQueue {};
    std::unordered_map<std::basic_string<char>, std::vector<ProcessInfo>> suspendedProcesses {};
    std::unordered_map<std::basic_string<char>, std::vector<ProcessInfo>> resumedProcesses {};

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        auto runningProcesses = MUST(get_running_processes());

        ProcessListenerContext processListenerContext {
            processCreationListener,
            processDeletionListener,
            protectedProcesses,
            runningProcesses,
            suspensionQueue,
            suspendedProcesses,
            resumedProcesses
        };

        process_creation_handler(processListenerContext);
        process_deletion_handler(processListenerContext);
        process_suspension_handler(processListenerContext);

        ImGui::SetNextWindowPos({});
        ImGui::SetNextWindowSize({ static_cast<float>(width), static_cast<float>(height) });
        ImGui::Begin("Locker", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

        if (!suspendedProcesses.empty())
        {
            ImGui::OpenPopup("unlock_program_popup");
        }

        if (ImGui::BeginPopup("unlock_program_popup"))
        {
            static char password[256] = {};
            auto& processInfo = (processListenerContext.suspendedProcesses | std::views::values).front();
            ImGui::Text("Type the password for %s", processInfo.front().name.data());
            ImGui::Separator();
            ImGui::Text("Password");
            ImGui::InputText("##password", password, sizeof(password));
            if (ImGui::Button("Unlock"))
            {
                process_resumption_handler(processListenerContext, password);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::BeginGroup();
            static char searchProcessName[MAX_PATH] = {};
            static std::string previousSearchProcessName {};
            ImGui::Text("Search Process");
            ImGui::SetNextItemWidth(400);
            ImGui::InputText("##search_process", searchProcessName, sizeof(searchProcessName));

            auto searchProcessNameFixed = std::string_view(searchProcessName) | std::views::transform(tolower) | std::ranges::to<std::string>();
            auto filteredProcesses = std::views::filter(runningProcesses, [&searchProcessNameFixed] (std::pair<std::string, std::vector<ProcessInfo>> const& lhs) {
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
                        selectedProcessName = process.first;
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
                        protectedProcesses.insert({ selectedProcessName, password });
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

                for (auto const& [rowIndex, value] : std::views::enumerate(protectedProcesses))
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
