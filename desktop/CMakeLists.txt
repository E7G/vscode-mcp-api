cmake_minimum_required(VERSION 4.2)
project(vscode_mcp_qt VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Widgets Network)

qt_add_executable(vscode-mcp WIN32
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/mcpserver.cpp
    src/mcpserver.h
    src/editorbridge.cpp
    src/editorbridge.h
    src/languageservice.cpp
    src/languageservice.h
    src/ngrokmanager.cpp
    src/ngrokmanager.h
    src/credentials.cpp
    src/credentials.h
)

target_link_libraries(vscode-mcp PRIVATE Qt6::Core Qt6::Widgets Qt6::Network)
target_compile_definitions(vscode-mcp PRIVATE APP_VERSION="${PROJECT_VERSION}")

if(WIN32)
    target_link_libraries(vscode-mcp PRIVATE Advapi32 Shell32)
endif()

install(TARGETS vscode-mcp RUNTIME DESTINATION .)
