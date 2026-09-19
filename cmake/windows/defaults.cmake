# CMake Windows defaults module

include_guard(GLOBAL)

# Enable find_package targets to become globally available targets
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)

include(buildspec)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  file(TO_CMAKE_PATH "$ENV{ALLUSERSPROFILE}" allusersprofile_path)
  set(
    CMAKE_INSTALL_PREFIX
    "${allusersprofile_path}/obs-studio/plugins"
    CACHE STRING
    "Default plugin installation directory"
    FORCE
  )
endif()
