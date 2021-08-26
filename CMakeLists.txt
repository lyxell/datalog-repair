cmake_minimum_required(VERSION 3.18)

project(logifix HOMEPAGE_URL "https://github.com/lyxell/logifix")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

### DEPENDENCIES

find_package(BISON 3.5.1 REQUIRED)

add_subdirectory(vendor/fmt)
add_subdirectory(vendor/nway)
add_subdirectory(vendor/souffle)
include_directories(vendor/souffle/src/include)

### PARSER

bison_target(parser src/parser/parser.yy parser.cpp)
add_custom_command(
  OUTPUT lexer.cpp
  COMMAND re2c -W --input-encoding utf8 -i ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/lexer.re2c.cpp -o lexer.cpp
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/lexer.re2c.cpp
  VERBATIM)

### SOURCE

#### Set version
find_package(Git)
if(GIT_EXECUTABLE)
  # Generate a git-describe version string from Git repository tags
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --match "v*"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_DESCRIBE_VERSION
    RESULT_VARIABLE GIT_DESCRIBE_ERROR_CODE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    )
  if(NOT GIT_DESCRIBE_ERROR_CODE)
    set(LOGIFIX_VERSION ${GIT_DESCRIBE_VERSION})
  endif()
endif()
if(NOT DEFINED LOGIFIX_VERSION)
  set(LOGIFIX_VERSION v0.0.0-unknown)
  message(WARNING "Failed to determine LOGIFIX_VERSION from Git tags. Using default version \"${LOGIFIX_VERSION}\".")
endif()
message(STATUS "Building logifix version ${LOGIFIX_VERSION}")

#### Find files
file(GLOB_RECURSE RULE_IMPLEMENTATION_FILES src/rules/*.dl)
file(GLOB_RECURSE RULE_DATA_FILES src/rules/*.json)

#### Generate logifix.cpp from Datalog code
add_custom_command(
  OUTPUT logifix.cpp
  COMMAND souffle -PSIPS:strict --generate=logifix ${CMAKE_CURRENT_SOURCE_DIR}/src/program.dl
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/program.dl
  DEPENDS ${RULE_IMPLEMENTATION_FILES}
  VERBATIM)
set_source_files_properties(logifix.cpp PROPERTIES COMPILE_FLAGS -D__EMBEDDED_SOUFFLE__)

#### Generate rule_data.cpp from json files
add_custom_command(
  OUTPUT rule_data.cpp
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_rule_data.sh rule_data.cpp
  DEPENDS ${RULE_DATA_FILES}
  VERBATIM)

#### Generate config.h from config.h.in
configure_file(src/config.h.in ${CMAKE_BINARY_DIR}/config.h)

#### Create executable
add_executable(logifix src/cli/cli.cpp src/cli/tty.cpp src/parser/javadoc.cpp src/logifix.cpp src/functors.cpp src/utils.cpp src/timer.cpp logifix.cpp rule_data.cpp parser.cpp lexer.cpp)
target_include_directories(logifix PRIVATE ${CMAKE_BINARY_DIR})
target_include_directories(logifix PRIVATE ${CMAKE_SOURCE_DIR}/src/parser)
target_include_directories(logifix PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(logifix pthread nway fmt)
if(UNIX AND NOT APPLE)
    target_link_libraries(logifix -static-libgcc -static-libstdc++)
endif()

### TESTS

enable_testing()
add_subdirectory(test)
