cmake_minimum_required(VERSION 3.25)

project(locker LANGUAGES CXX C)

include(cmake/static_analyzers.cmake)
include(cmake/get_cpm.cmake)

set(locker_CompilerOptions ${locker_CompilerOptions}
    -Wno-gnu-statement-expression-from-macro-expansion
    -Wno-language-extension-token
)
# set(locker_LinkerOptions ${locker_LinkerOptions})

CPMAddPackage("gh:fmtlib/fmt#10.2.1")
CPMAddPackage("gh:glfw/glfw#3.4")
CPMAddPackage("gh:gabime/spdlog#v1.15.2")

CPMFindPackage(
    NAME expected
    GITHUB_REPOSITORY nyyakko/expected
    GIT_TAG master
)

CPMFindPackage(
    NAME LibError
    GITHUB_REPOSITORY nyyakko/LibError
    GIT_TAG master
)

set(locker_ExternalLibraries
    spdlog::spdlog
    fmt::fmt
    glfw
    opengl32
    wbemuuid
    ole32
    comsupp
    LibError::LibError
)

add_subdirectory(locker)

