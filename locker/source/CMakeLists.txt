add_subdirectory(os)
add_subdirectory(imgui)
add_subdirectory(glad)

set(DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(locker_SourceFiles ${locker_SourceFiles}
    "${DIR}/Main.cpp"

    PARENT_SCOPE
)

