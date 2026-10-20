# file      : CMakeLists.txt
# license   : GNU GPL v3; see accompanying LICENSE file
#
# CMake build for the Clang-based ODB compiler. Produces a single,
# self-contained odb executable with the Clang frontend and resource
# headers linked/embedded in (no GCC plugin, no external compiler needed
# for parsing).
#
# Requirements: static Clang/LLVM libraries (for example, MSYS2
# mingw-w64-ucrt-x86_64-clang/llvm on Windows or distribution's
# clang/llvm static libraries on Linux).
#
# Typical configuration:
#
#   cmake -G Ninja -S odb/odb -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#

cmake_minimum_required (VERSION 3.16)

project (odb VERSION 2.6.0 LANGUAGES C CXX)

set (CMAKE_CXX_STANDARD 17)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_EXTENSIONS OFF)

# By default use the git submodules in deps/ (git submodule update --init).
#
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../deps/libcutl/libcutl")
  set (ODB_DEPS_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../deps")
else ()
  set (ODB_DEPS_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../../deps")
endif ()

set (ODB_DEPS_DIR "${ODB_DEPS_DEFAULT}" CACHE PATH
     "Directory containing libcutl, libstudxml, and libstud-optional")

option (ODB_STATIC "Link the odb executable statically" ON)

find_program (LLVM_CONFIG NAMES llvm-config REQUIRED)

execute_process (COMMAND "${LLVM_CONFIG}" --includedir
                 OUTPUT_VARIABLE LLVM_INCLUDE_DIR
                 OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process (COMMAND "${LLVM_CONFIG}" --libdir
                 OUTPUT_VARIABLE LLVM_LIB_DIR
                 OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process (COMMAND "${LLVM_CONFIG}" --version
                 OUTPUT_VARIABLE LLVM_VERSION
                 OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process (COMMAND "${LLVM_CONFIG}" --link-static --libs
                         windowsdriver frontendopenmp frontendhlsl option
                         core support bitreader mc mcparser profiledata
                         targetparser demangle binaryformat remarks
                         bitstreamreader passes plugins
                 OUTPUT_VARIABLE LLVM_LIBS
                 OUTPUT_STRIP_TRAILING_WHITESPACE)
separate_arguments (LLVM_LIBS UNIX_COMMAND "${LLVM_LIBS}")

string (REGEX MATCH "^[0-9]+" LLVM_VERSION_MAJOR "${LLVM_VERSION}")

# Clang resource directory (builtin headers) that we embed into the
# executable.
#
set (CLANG_RESOURCE_DIR "${LLVM_LIB_DIR}/clang/${LLVM_VERSION_MAJOR}"
     CACHE PATH "Clang resource directory")

if (NOT EXISTS "${CLANG_RESOURCE_DIR}/include/stddef.h")
  message (FATAL_ERROR "Clang resource headers not found in "
           "${CLANG_RESOURCE_DIR}/include")
endif ()

message (STATUS "LLVM ${LLVM_VERSION} in ${LLVM_LIB_DIR}")
message (STATUS "Clang resource directory: ${CLANG_RESOURCE_DIR}")

set (CLANG_LIBS
  clangFrontend clangDriver clangOptions clangParse clangSerialization
  clangSema clangAPINotes clangEdit clangAnalysisLifetimeSafety
  clangAnalysis clangASTMatchers clangAST clangSupport clangLex clangBasic)

#
# Dependencies.
#

# Generate version headers from the .in templates (build2 substitution
# syntax: $name$).
#
function (odb_configure_version in out name ver)
  string (REPLACE "." ";" vl "${ver}")
  list (GET vl 0 major)
  list (GET vl 1 minor)
  list (GET vl 2 patch)
  math (EXPR pn "${major} * 10000000000 + ${minor} * 100000 + ${patch}")
  set (num "${pn}0000")

  file (READ "${in}" c)
  string (REPLACE "$${name}.version.project_number$" "${num}" c "${c}")
  string (REPLACE "$${name}.version.project_id$" "${ver}" c "${c}")
  string (REPLACE "$${name}.version.project$" "${ver}" c "${c}")
  string (REPLACE "$${name}.version.major$" "${major}" c "${c}")
  string (REPLACE "$${name}.version.minor$" "${minor}" c "${c}")
  string (REPLACE "$${name}.version.patch$" "${patch}" c "${c}")
  string (REPLACE "$${name}.version.pre_release$" "false" c "${c}")
  string (REPLACE "$${name}.version.snapshot_sn$" "0" c "${c}")
  string (REPLACE "$${name}.version.snapshot_id$" "" c "${c}")
  string (REPLACE "$${name}.version$" "${ver}" c "${c}")
  file (WRITE "${out}.tmp" "${c}")
  configure_file ("${out}.tmp" "${out}" COPYONLY)
endfunction ()

set (GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")

odb_configure_version ("${ODB_DEPS_DIR}/libcutl/libcutl/version.hxx.in"
                       "${GEN_DIR}/libcutl/version.hxx" libcutl 1.11.0)
odb_configure_version ("${ODB_DEPS_DIR}/libstudxml/libstudxml/version.hxx.in"
                       "${GEN_DIR}/libstudxml/version.hxx" libstudxml 1.1.0)
odb_configure_version ("${CMAKE_CURRENT_SOURCE_DIR}/odb/version.hxx.in"
                       "${GEN_DIR}/odb/version.hxx" odb ${PROJECT_VERSION})

file (GLOB_RECURSE CUTL_SOURCES "${ODB_DEPS_DIR}/libcutl/libcutl/*.cxx")

add_library (cutl STATIC ${CUTL_SOURCES})
target_include_directories (cutl BEFORE PUBLIC "${GEN_DIR}" "${ODB_DEPS_DIR}/libcutl")
target_compile_definitions (cutl PRIVATE LIBCUTL_STATIC_BUILD PUBLIC LIBCUTL_STATIC)

set (XML_DIR "${ODB_DEPS_DIR}/libstudxml/libstudxml")

add_library (studxml STATIC
  "${XML_DIR}/parser.cxx"
  "${XML_DIR}/qname.cxx"
  "${XML_DIR}/serializer.cxx"
  "${XML_DIR}/value-traits.cxx"
  "${XML_DIR}/details/expat/xmlparse.c"
  "${XML_DIR}/details/expat/xmlrole.c"
  "${XML_DIR}/details/expat/xmltok.c"
  "${XML_DIR}/details/genx/genx.c"
  "${XML_DIR}/details/genx/char-props.c")
target_include_directories (studxml BEFORE PUBLIC "${GEN_DIR}" "${ODB_DEPS_DIR}/libstudxml")
target_compile_definitions (studxml PRIVATE LIBSTUDXML_STATIC_BUILD PUBLIC LIBSTUDXML_STATIC)

#
# Embedded Clang resource headers.
#

file (GLOB_RECURSE CLANG_RESOURCE_HEADERS RELATIVE "${CLANG_RESOURCE_DIR}"
      "${CLANG_RESOURCE_DIR}/include/*")

set (RESOURCE_CXX "${CMAKE_CURRENT_BINARY_DIR}/resource-headers.cxx")

add_custom_command (
  OUTPUT "${RESOURCE_CXX}"
  COMMAND "${CMAKE_COMMAND}"
          "-DRESOURCE_DIR=${CLANG_RESOURCE_DIR}"
          "-DOUTPUT=${RESOURCE_CXX}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/embed-resources.cmake"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/embed-resources.cmake"
  COMMENT "Embedding Clang resource headers"
  VERBATIM)

#
# The odb compiler.
#

set (ODB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/odb")

file (GLOB_RECURSE ODB_SOURCES RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
      "${ODB_DIR}/*.cxx")
list (FILTER ODB_SOURCES EXCLUDE REGEX "^odb/options\\.cxx$")

file (READ "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" ODB_LICENSE)
string (REGEX MATCH "Copyright \\(c\\) ([^\n]*)\\." _ "${ODB_LICENSE}")
set (ODB_COPYRIGHT "${CMAKE_MATCH_1}")

add_executable (odb ${ODB_SOURCES} "${RESOURCE_CXX}")

target_include_directories (odb BEFORE PRIVATE
  "${GEN_DIR}"
  "${ODB_DIR}/pregenerated"
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${ODB_DEPS_DIR}/libstud-optional")

target_include_directories (odb SYSTEM PRIVATE "${LLVM_INCLUDE_DIR}")

target_compile_definitions (odb PRIVATE
  "ODB_COPYRIGHT=\"${ODB_COPYRIGHT}\""
  "ODB_CLANG_VERSION=\"${LLVM_VERSION}\""
  "ODB_CLANG_VERSION_MAJOR=\"${LLVM_VERSION_MAJOR}\""
  __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS __STDC_LIMIT_MACROS)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options (odb PRIVATE -Wno-deprecated-declarations)
endif ()

target_link_directories (odb PRIVATE "${LLVM_LIB_DIR}")

set (ODB_SYSTEM_LIBS)

if (WIN32)
  list (APPEND ODB_SYSTEM_LIBS psapi shell32 ole32 uuid advapi32 ws2_32
        ntdll version)
else ()
  list (APPEND ODB_SYSTEM_LIBS pthread dl)
endif ()

find_library (ZLIB_STATIC NAMES libz.a z)
find_library (ZSTD_STATIC NAMES libzstd.a zstd)

if (ZLIB_STATIC)
  list (APPEND ODB_SYSTEM_LIBS "${ZLIB_STATIC}")
endif ()

if (ZSTD_STATIC)
  list (APPEND ODB_SYSTEM_LIBS "${ZSTD_STATIC}")
endif ()

target_link_libraries (odb PRIVATE
  cutl studxml
  -Wl,--start-group ${CLANG_LIBS} ${LLVM_LIBS} -Wl,--end-group
  ${ODB_SYSTEM_LIBS})

if (ODB_STATIC)
  target_link_options (odb PRIVATE -static)
endif ()

# Strip the (otherwise huge) executable in the release builds.
#
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_link_options (odb PRIVATE $<$<CONFIG:Release,MinSizeRel>:-s>)
endif ()

# On MinGW the Clang libraries may be compiled with Clang which emits some
# function-local statics in libstdc++ templates (for example, in <regex>)
# into differently-named COMDAT sections than GCC. The definitions are
# identical so let the linker pick the first one.
#
if (MINGW)
  target_link_options (odb PRIVATE -Wl,--allow-multiple-definition)
endif ()

install (TARGETS odb RUNTIME DESTINATION bin)
