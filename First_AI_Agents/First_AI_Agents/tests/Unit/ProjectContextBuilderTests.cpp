#include "ai/orchestration/ProjectContextBuilder.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;

struct TempProject {
    fs::path root;
    fs::path outside;

    TempProject()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("ai_orchestrator_context_test_" + std::to_string(stamp));
        outside = fs::temp_directory_path() / ("ai_orchestrator_context_test_outside_" + std::to_string(stamp));
        fs::create_directories(root);
        fs::create_directories(outside);
    }

    ~TempProject()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
        fs::remove_all(outside, ignored);
    }
};

void writeFile(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << content;
}

void assertThrowsContaining(const std::function<void()>& action, const std::string& expected)
{
    bool thrown = false;
    try {
        action();
    } catch (const std::exception& ex) {
        thrown = true;
        assert(std::string {ex.what()}.find(expected) != std::string::npos);
    }

    assert(thrown);
}

ai::orchestration::ProjectContextSelection selectionFor(const std::string& path)
{
    return {
        "test selection",
        {path},
        {},
        {}
    };
}

} // namespace

int runProjectContextBuilderTests()
{
    {
        TempProject project;
        writeFile(project.root / "sourceFiles" / "orchestration" / "ProjectContextBuilder.cpp", "int value = 1;\n");

        const ai::orchestration::ProjectContext context =
            ai::orchestration::ProjectContextBuilder(project.root).buildSelected(
                selectionFor("sourceFiles/orchestration/ProjectContextBuilder.cpp"));

        assert(context.files.size() == 1);
        assert(context.files.front().relativePath == "sourceFiles/orchestration/ProjectContextBuilder.cpp");
        assert(context.manifest.find("[EDITABLE] sourceFiles/orchestration/ProjectContextBuilder.cpp") != std::string::npos);
        assert(context.context.find("int value = 1;") != std::string::npos);
    }

    {
        TempProject project;
        assertThrowsContaining([&project] {
            const auto ignored =
                ai::orchestration::ProjectContextBuilder(project.root).buildSelected(selectionFor("missing.cpp"));
            static_cast<void>(ignored);
        }, "cannot resolve");
    }

    {
        TempProject project;
        writeFile(project.root / "workspace" / "output.txt", "previous run output\n");
        assertThrowsContaining([&project] {
            const auto ignored =
                ai::orchestration::ProjectContextBuilder(project.root).buildSelected(selectionFor("workspace/output.txt"));
            static_cast<void>(ignored);
        }, "excluded");
    }

    {
        TempProject project;
        writeFile(project.root / "config" / "api_key.txt", "not-a-real-key\n");
        assertThrowsContaining([&project] {
            const auto ignored =
                ai::orchestration::ProjectContextBuilder(project.root).buildSelected(selectionFor("config/api_key.txt"));
            static_cast<void>(ignored);
        }, "secret");
    }

    {
        TempProject project;
        writeFile(project.root / "assets" / "image.bin", "binary-ish\n");
        assertThrowsContaining([&project] {
            const auto ignored =
                ai::orchestration::ProjectContextBuilder(project.root).buildSelected(selectionFor("assets/image.bin"));
            static_cast<void>(ignored);
        }, "file type");
    }

    {
        TempProject project;
        writeFile(project.root / "large.txt", std::string(512U * 1024U + 1U, 'x'));
        assertThrowsContaining([&project] {
            const auto ignored =
                ai::orchestration::ProjectContextBuilder(project.root).buildSelected(selectionFor("large.txt"));
            static_cast<void>(ignored);
        }, "exceeds");
    }

    {
        TempProject project;
        writeFile(project.outside / "outside.cpp", "int outside = 1;\n");

        std::error_code error;
        fs::create_symlink(project.outside / "outside.cpp", project.root / "linked.cpp", error);
        if (!error) {
            assertThrowsContaining([&project] {
                const auto ignored =
                    ai::orchestration::ProjectContextBuilder(project.root).buildSelected(selectionFor("linked.cpp"));
                static_cast<void>(ignored);
            }, "outside the project root");
        }
    }

    return 0;
}
