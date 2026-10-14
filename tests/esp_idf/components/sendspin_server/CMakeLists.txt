# The library as the component sendspin_server, with the repository's Kconfig (Kconfig here): a
# component's name is its directory's, which for the repository's root depends on the checkout
include(${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/esp_idf.cmake)

# Warnings fail the build, as SENDSPIN_SERVER_ENABLE_WERROR does on a host
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Werror)
endif()
