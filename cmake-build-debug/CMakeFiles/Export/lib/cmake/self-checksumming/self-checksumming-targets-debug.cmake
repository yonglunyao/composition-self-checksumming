#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "self-checksumming::SCPass" for configuration "Debug"
set_property(TARGET self-checksumming::SCPass APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(self-checksumming::SCPass PROPERTIES
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libSCPass.so"
  IMPORTED_SONAME_DEBUG "libSCPass.so"
  )

list(APPEND _IMPORT_CHECK_TARGETS self-checksumming::SCPass )
list(APPEND _IMPORT_CHECK_FILES_FOR_self-checksumming::SCPass "${_IMPORT_PREFIX}/lib/libSCPass.so" )

# Import target "self-checksumming::SCPatchPass" for configuration "Debug"
set_property(TARGET self-checksumming::SCPatchPass APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(self-checksumming::SCPatchPass PROPERTIES
  IMPORTED_COMMON_LANGUAGE_RUNTIME_DEBUG ""
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libSCPatchPass.so"
  IMPORTED_NO_SONAME_DEBUG "TRUE"
  )

list(APPEND _IMPORT_CHECK_TARGETS self-checksumming::SCPatchPass )
list(APPEND _IMPORT_CHECK_FILES_FOR_self-checksumming::SCPatchPass "${_IMPORT_PREFIX}/lib/libSCPatchPass.so" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
