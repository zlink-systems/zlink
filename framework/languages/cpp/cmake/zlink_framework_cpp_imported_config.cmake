include_guard(GLOBAL)

function(zlink_framework_cpp_configure_imported_target target_name)
  if(NOT TARGET "${target_name}")
    message(FATAL_ERROR
      "Cannot configure imported target '${target_name}': the target does not exist")
  endif()

  get_target_property(_zlink_target_is_imported "${target_name}" IMPORTED)
  if(NOT _zlink_target_is_imported)
    return()
  endif()

  get_target_property(_zlink_imported_configurations
    "${target_name}" IMPORTED_CONFIGURATIONS)
  if(NOT _zlink_imported_configurations)
    return()
  endif()

  set(_zlink_available_configurations)
  foreach(_zlink_configuration IN LISTS _zlink_imported_configurations)
    string(TOUPPER "${_zlink_configuration}" _zlink_configuration_upper)
    list(APPEND _zlink_available_configurations "${_zlink_configuration_upper}")
  endforeach()
  list(REMOVE_DUPLICATES _zlink_available_configurations)

  set(_zlink_requested_configuration "${CMAKE_BUILD_TYPE}")
  if(NOT _zlink_requested_configuration)
    if(NOT CMAKE_CONFIGURATION_TYPES)
      return()
    endif()

    set(_zlink_missing_generator_configurations)
    foreach(_zlink_configuration IN LISTS CMAKE_CONFIGURATION_TYPES)
      string(TOUPPER "${_zlink_configuration}" _zlink_configuration_upper)
      if(NOT _zlink_configuration_upper IN_LIST _zlink_available_configurations)
        list(APPEND _zlink_missing_generator_configurations "${_zlink_configuration}")
      endif()
    endforeach()
    if(NOT _zlink_missing_generator_configurations)
      return()
    endif()

    list(JOIN _zlink_imported_configurations ", " _zlink_available_text)
    message(FATAL_ERROR
      "Imported target '${target_name}' provides only [${_zlink_available_text}], "
      "but the multi-config generator has no requested configuration. "
      "Set CMAKE_BUILD_TYPE to the configuration that will be built.")
  endif()

  string(TOUPPER "${_zlink_requested_configuration}"
    _zlink_requested_configuration_upper)
  if(NOT _zlink_requested_configuration_upper IN_LIST _zlink_available_configurations)
    list(JOIN _zlink_imported_configurations ", " _zlink_available_text)
    message(FATAL_ERROR
      "Imported target '${target_name}' does not provide the requested "
      "configuration '${_zlink_requested_configuration}'. Available "
      "configurations: [${_zlink_available_text}]. Install the matching "
      "zlink_cpp package configuration before configuring the Framework.")
  endif()

  if(CMAKE_CONFIGURATION_TYPES)
    foreach(_zlink_configuration IN LISTS CMAKE_CONFIGURATION_TYPES)
      string(TOUPPER "${_zlink_configuration}" _zlink_configuration_upper)
      if(NOT _zlink_configuration_upper IN_LIST
          _zlink_available_configurations)
        set_property(TARGET "${target_name}" PROPERTY
          "MAP_IMPORTED_CONFIG_${_zlink_configuration_upper}"
          "${_zlink_requested_configuration}")
      endif()
    endforeach()
  endif()
endfunction()
