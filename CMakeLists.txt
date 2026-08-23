cmake_minimum_required(VERSION 3.20)
project(ticket_booking_system LANGUAGES CXX)

option(USE_CROW "Download and build Crow HTTP library" ON)

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(USE_CROW)
    set(CROW_USE_BOOST ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        Crow
        GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
        GIT_TAG v1.2.1
    )
    FetchContent_MakeAvailable(Crow)

endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(booking_core
    src/booking_service.cpp
    src/db_booking_service.cpp
    src/db.cpp
    src/auth.cpp
    src/email.cpp
)
target_include_directories(booking_core PUBLIC include src)

find_package(OpenSSL REQUIRED)

target_link_libraries(booking_core PUBLIC OpenSSL::Crypto)

if(BUILD_TESTING)
    enable_testing()
    add_executable(booking_core_tests tests/booking_service_tests.cpp)
    target_link_libraries(booking_core_tests PRIVATE booking_core)
    target_compile_definitions(booking_core_tests PRIVATE PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME booking_core_tests COMMAND booking_core_tests)

    add_executable(auth_tests tests/auth_tests.cpp)
    target_link_libraries(auth_tests PRIVATE booking_core OpenSSL::Crypto)
    target_compile_definitions(auth_tests PRIVATE PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME auth_tests COMMAND auth_tests)

    add_executable(db_booking_service_tests tests/db_booking_service_tests.cpp)
    target_link_libraries(db_booking_service_tests PRIVATE booking_core SQLite::SQLite3 OpenSSL::Crypto)
    target_compile_definitions(db_booking_service_tests PRIVATE PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME db_booking_service_tests COMMAND db_booking_service_tests)

    add_executable(email_tests tests/email_tests.cpp)
    target_link_libraries(email_tests PRIVATE booking_core)
    target_compile_definitions(email_tests PRIVATE PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME email_tests COMMAND email_tests)
endif()

find_package(SQLite3 REQUIRED)

if(USE_CROW)
    add_executable(ticket_booking_app src/main.cpp)
    target_link_libraries(ticket_booking_app PRIVATE booking_core Crow::Crow SQLite::SQLite3)
else()
    message(STATUS "USE_CROW is OFF: not building ticket_booking_app (server) target")
endif()
