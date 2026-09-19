cmake_minimum_required(VERSION 3.24)

# The seven samples as one build against an installed zlink_framework package.
# This is the entry point of the downloaded samples archive: bootstrap.cmake
# installs the published framework beside this file and configures this
# project, and every samples/<Sample>/run_sample.{sh,ps1} builds its own
# targets out of the one build tree (`build/`). Inside the repository the
# framework's own CMakeLists.txt adds each sample directly instead, so this
# file is never consumed there.
project(ZlinkSamplesCpp LANGUAGES C CXX)

set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
find_package(zlink CONFIG REQUIRED)
find_package(zlink_framework CONFIG REQUIRED)
set(ZLINK_FRAMEWORK_CPP_SAMPLES_PACKAGE_MODE ON)
message(STATUS "Zlink sample package mode: ${ZLINK_FRAMEWORK_CPP_SAMPLES_PACKAGE_MODE}")

foreach(_sample Bingo DeliveryDispatch GameQuest ShoppingMall SupportChat TicTacToe ZoneWorld)
  set(CMAKE_FOLDER "samples/${_sample}")
  add_subdirectory(${_sample})
endforeach()
