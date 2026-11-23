cmake_minimum_required(VERSION 3.20)

project(SharedLedgerSkeleton VERSION 0.1 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This project requires Windows 11.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

include(CTest)

add_library(ledger_common INTERFACE)
target_include_directories(
    ledger_common
    INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_library(ledger_datastore STATIC
    src/datastore/FileDataStore.cpp
    src/datastore/FileDataStore.h
    src/datastore/JsonSnapshot.cpp
    src/datastore/JsonSnapshot.h
)
target_link_libraries(ledger_datastore PUBLIC ledger_common)
target_compile_definitions(ledger_datastore PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

add_library(ledger_service STATIC
    src/service/LedgerService.cpp
    src/service/LedgerService.h
)
target_include_directories(ledger_service PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(ledger_service PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(
    ledger_service
    PUBLIC ledger_common
    PRIVATE ledger_datastore
)

add_executable(ledger_ui WIN32
    src/ui/main.cpp
    src/ui/OperationsDialog.cpp
    src/ui/OperationsDialog.h
    src/ui/Win32FileDialog.cpp
    src/ui/Win32FileDialog.h
)
target_compile_definitions(
    ledger_ui
    PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
target_link_libraries(ledger_ui PRIVATE ledger_service Comdlg32)

add_library(ledger_console_ui STATIC
    src/ui/ConsoleController.cpp
    src/ui/ConsoleController.h
)
target_include_directories(ledger_console_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(ledger_console_ui PRIVATE ledger_service)
target_compile_definitions(
    ledger_console_ui
    PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
)

add_executable(ledger_console src/ui/console_main.cpp)
target_link_libraries(ledger_console PRIVATE ledger_console_ui ledger_service)

add_library(ledger_ui_paths STATIC
    src/ui/ProjectPaths.cpp
    src/ui/ProjectPaths.h
)
target_include_directories(ledger_ui_paths PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(ledger_ui_paths PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(ledger_ui PRIVATE ledger_ui_paths)

if(MSVC)
    target_compile_options(ledger_datastore PRIVATE /W4 /permissive-)
    target_compile_options(ledger_service PRIVATE /W4 /permissive-)
    target_compile_options(ledger_ui PRIVATE /W4 /permissive-)
    target_compile_options(ledger_console_ui PRIVATE /W4 /permissive-)
    target_compile_options(ledger_console PRIVATE /W4 /permissive-)
    target_compile_options(ledger_ui_paths PRIVATE /W4 /permissive-)
endif()

if(BUILD_TESTING)
    add_executable(ledger_tests tests/empty_test.cpp)
    target_link_libraries(ledger_tests PRIVATE ledger_service)
    add_executable(ledger_service_tests tests/service_membership_tests.cpp)
    add_executable(ledger_review_tests tests/service_review_tests.cpp)
    add_executable(ledger_own_entries_tests tests/service_own_entries_tests.cpp)
    add_executable(ledger_query_tests tests/service_query_tests.cpp)
    add_executable(ledger_console_tests tests/console_ui_tests.cpp)
    add_executable(ledger_file_persistence_tests tests/file_persistence_tests.cpp)
    add_executable(ledger_ui_paths_tests tests/ui_paths_tests.cpp)
    add_executable(ledger_entry_editing_tests tests/service_entry_editing_tests.cpp)
    target_include_directories(
        ledger_service_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ledger_service_tests PRIVATE ledger_service)
    target_include_directories(
        ledger_review_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ledger_review_tests PRIVATE ledger_service)
    target_include_directories(
        ledger_own_entries_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ledger_own_entries_tests PRIVATE ledger_service)
    target_include_directories(
        ledger_query_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ledger_query_tests PRIVATE ledger_service)
    target_link_libraries(
        ledger_console_tests
        PRIVATE ledger_console_ui ledger_service
    )
    target_include_directories(
        ledger_file_persistence_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        ledger_file_persistence_tests
        PRIVATE ledger_service ledger_datastore
    )
    target_link_libraries(ledger_ui_paths_tests PRIVATE ledger_ui_paths)
    target_include_directories(
        ledger_entry_editing_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ledger_entry_editing_tests PRIVATE ledger_service)
    if(MSVC)
        target_compile_options(ledger_tests PRIVATE /W4 /permissive-)
        target_compile_options(ledger_service_tests PRIVATE /W4 /permissive-)
        target_compile_options(ledger_review_tests PRIVATE /W4 /permissive-)
        target_compile_options(ledger_own_entries_tests PRIVATE /W4 /permissive-)
        target_compile_options(ledger_query_tests PRIVATE /W4 /permissive-)
        target_compile_options(ledger_console_tests PRIVATE /W4 /permissive-)
        target_compile_options(ledger_file_persistence_tests PRIVATE /W4 /permissive-)
        target_compile_options(ledger_ui_paths_tests PRIVATE /W4 /permissive-)
        target_compile_options(ledger_entry_editing_tests PRIVATE /W4 /permissive-)
    endif()
    add_test(NAME ledger.empty COMMAND ledger_tests)
    add_test(NAME ledger.service.membership COMMAND ledger_service_tests)
    add_test(NAME ledger.service.review COMMAND ledger_review_tests)
    add_test(NAME ledger.service.own_entries COMMAND ledger_own_entries_tests)
    add_test(NAME ledger.service.query COMMAND ledger_query_tests)
    add_test(NAME ledger.ui.console COMMAND ledger_console_tests)
    add_test(NAME ledger.persistence.file COMMAND ledger_file_persistence_tests)
    set_tests_properties(
        ledger.persistence.file
        PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME ledger.ui.paths COMMAND ledger_ui_paths_tests)
    add_test(NAME ledger.service.entry_editing COMMAND ledger_entry_editing_tests)
endif()
