# SPDX-License-Identifier: Apache-2.0

# Lynxi SoC family CMakeLists.txt
# Just pass through to the SoC series

if(DEFINED SOC_SERIES)
  add_subdirectory(${SOC_SERIES})
else()
  add_subdirectory(${SOC_NAME})
endif()