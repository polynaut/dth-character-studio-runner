cmake_minimum_required(VERSION 3.22)

project(DthJobRunner CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Parser tests (no Qt, no Daz SDK — always built; this is all CI runs) ────

enable_testing()
add_executable(test_csvjobfile
	tests/test_csvjobfile.cpp
	src/CsvJobFile.cpp
	src/CsvJobFile.h
)
target_include_directories(test_csvjobfile PRIVATE src)
add_test(NAME csvjobfile COMMAND test_csvjobfile)

# ─── Plugin (requires a Daz Studio SDK; configure with -DDAZ_SDK_DIR=…) ──────

set(DAZ_SDK_DIR "" CACHE PATH "Path to root of the DAZ Studio SDK (4.5+ or 6.x)")
if(NOT DAZ_SDK_DIR)
	message(STATUS "DAZ_SDK_DIR not set — building parser tests only")
	return()
endif()

if(NOT WIN32)
	message(FATAL_ERROR "The plugin build currently targets Windows only")
endif()

set(DAZ_SDK_VERSION "6" CACHE STRING "Target DAZ Studio SDK major version (4 or 6)")
set_property(CACHE DAZ_SDK_VERSION PROPERTY STRINGS "4" "6")
if(NOT DAZ_SDK_VERSION STREQUAL "4" AND NOT DAZ_SDK_VERSION STREQUAL "6")
	message(FATAL_ERROR "DAZ_SDK_VERSION must be '4' or '6', got '${DAZ_SDK_VERSION}'")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "Configure a 64-bit build (-A x64)")
endif()
set(DZ_PLATFORM x64)

# SDK6 moved the platform directory above lib: x64/lib/dzcore.lib.
# SDK4 layout is lib/x64/dzcore.lib.
if(DAZ_SDK_VERSION STREQUAL "6")
	set(DAZ_SDK_LIB_DIR "${DAZ_SDK_DIR}/${DZ_PLATFORM}/lib")
else()
	set(DAZ_SDK_LIB_DIR "${DAZ_SDK_DIR}/lib/${DZ_PLATFORM}")
endif()

set(DAZ_SDK_INCLUDE "${DAZ_SDK_DIR}/include" CACHE PATH "Path to DAZ SDK includes")
set(DAZ_SDK_CORE_LIB "${DAZ_SDK_LIB_DIR}/dzcore.lib")
if(NOT EXISTS "${DAZ_SDK_CORE_LIB}")
	message(FATAL_ERROR "dzcore.lib not found at ${DAZ_SDK_CORE_LIB} — check DAZ_SDK_DIR/DAZ_SDK_VERSION")
endif()

# The SDK ships release-only libraries — use /MD and undef _DEBUG in every config.
add_compile_options("/MD" "/U_DEBUG")

add_library(dzcore SHARED IMPORTED)
set_property(TARGET dzcore APPEND PROPERTY IMPORTED_IMPLIB "${DAZ_SDK_CORE_LIB}")
set_property(TARGET dzcore APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${DAZ_SDK_INCLUDE}")

if(DAZ_SDK_VERSION STREQUAL "6")
	# ── SDK6: separate Qt6 devkit required (SDK ships no Qt at all). At
	# runtime the plugin resolves Qt6 against Daz Studio 6's own bundled DLLs
	# already loaded in-process. Install a devkit matching the studio's Qt
	# minor version (6.10.x for DS 6.25) — see README.
	set(DAZ_SDK_MEMORY_LIB "${DAZ_SDK_LIB_DIR}/dzsdkmemory.lib")
	if(NOT EXISTS "${DAZ_SDK_MEMORY_LIB}")
		message(FATAL_ERROR "dzsdkmemory.lib not found at ${DAZ_SDK_MEMORY_LIB} — check DAZ_SDK_DIR")
	endif()
	add_library(dzsdkmemory SHARED IMPORTED)
	set_property(TARGET dzsdkmemory APPEND PROPERTY IMPORTED_IMPLIB "${DAZ_SDK_MEMORY_LIB}")

	find_package(Qt6 REQUIRED COMPONENTS Core Core5Compat Gui Widgets Qml)
	# Qml: DzScript results are QJSValue on SDK6 (QtScript is gone).
	set(DZSDK_QT_TARGETS Qt6::Core Qt6::Core5Compat Qt6::Gui Qt6::Widgets Qt6::Qml)
else()
	# ── SDK4: Qt 4.8 is bundled inside the SDK itself.
	set(QT_QMAKE_EXECUTABLE   "${DAZ_SDK_DIR}/bin/${DZ_PLATFORM}/qmake.exe")
	set(QT_BINARY_DIR         "${DAZ_SDK_DIR}/bin/${DZ_PLATFORM}")
	set(QT_HEADERS_DIR        "${DAZ_SDK_INCLUDE}")
	set(QT_QTCORE_INCLUDE_DIR "${DAZ_SDK_INCLUDE}/QtCore")
	set(QT_QTCORE_LIBRARY_RELEASE "${DAZ_SDK_LIB_DIR}/QtCore4.lib")
	set(QT_IMPORTS_DIR        "${DAZ_SDK_LIB_DIR}")
	# The Qt4 find module insists on an mkspecs folder; fake it.
	file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/DUMMY_MKSPECS/default")
	set(QT_MKSPECS_DIR "${CMAKE_CURRENT_BINARY_DIR}/DUMMY_MKSPECS")

	find_package(Qt4 4.8.1 REQUIRED QtCore QtGui QtScript)
	set(DZSDK_QT_TARGETS Qt4::QtCore Qt4::QtGui Qt4::QtScript)
endif()

set(CMAKE_AUTOMOC ON)
list(APPEND CMAKE_AUTOMOC_MOC_OPTIONS -i)

add_library(dthjobrunner SHARED
	src/pluginmain.cpp
	src/JobPoller.cpp
	src/JobPoller.h
	src/JobRunnerAction.cpp
	src/JobRunnerAction.h
	src/CsvJobFile.cpp
	src/CsvJobFile.h
	src/version.h
)
target_include_directories(dthjobrunner PRIVATE src)
target_link_libraries(dthjobrunner PRIVATE dzcore ${DZSDK_QT_TARGETS})
target_compile_definitions(dthjobrunner PRIVATE DAZ_SDK_MAJOR_VERSION=${DAZ_SDK_VERSION})

# PDBs in every config so crashes in Daz resolve to symbols.
target_compile_options(dthjobrunner PRIVATE /Zi)
target_link_options(dthjobrunner PRIVATE /DEBUG /OPT:REF /OPT:ICF)

if(DAZ_SDK_VERSION STREQUAL "6")
	target_link_libraries(dthjobrunner PRIVATE dzsdkmemory)
	# DS6's plugin scanner only loads DLLs named dsp_* — without the prefix
	# the DLL is silently skipped at startup. DS4 has no such requirement.
	set(JOBRUNNER_OUTPUT_NAME "dsp_dthjobrunner")
else()
	set(JOBRUNNER_OUTPUT_NAME "dthjobrunner")
endif()

set_target_properties(dthjobrunner PROPERTIES
	OUTPUT_NAME "${JOBRUNNER_OUTPUT_NAME}"
	PREFIX ""
)
