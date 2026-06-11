# SPDX-License-Identifier: Apache-2.0

# Amlogic SoC family CMakeLists.txt
# Delegate to the SoC series

if(DEFINED SOC_SERIES)
  add_subdirectory(${SOC_SERIES})
else()
  add_subdirectory(${SOC_NAME})
endif()