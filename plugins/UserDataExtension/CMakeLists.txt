option(USER_DATA_EXTENSION_DEPLOY "Deploy UserDataExtension after building" ON)
set(USER_DATA_EXTENSION_DEPLOY_ROOT "" CACHE PATH "Development game root; override explicitly for final delivery")
configure_plugin_project(UserDataExtension NO_DEPLOY)
set(USER_DATA_EXTENSION_SOURCES UserDataConfiguration.cpp UserDataCollection.cpp UserDataFieldPath.cpp UserDataObjectBuilder.cpp UserDataPatchExecutor.cpp)
target_sources(UserDataExtension PRIVATE Plugin.cpp UserDataLoading.cpp ${USER_DATA_EXTENSION_SOURCES})
target_include_directories(UserDataExtension PRIVATE ${CMAKE_SOURCE_DIR}/plugins/ClassInjector/include)
target_compile_definitions(UserDataExtension PRIVATE NOMINMAX USER_DATA_EXTENSION_EXPORTS)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/UserDataExtension.toml" "# Zero-type provider: apply USER extensions before native onLoad.\n")
add_custom_command(TARGET UserDataExtension POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_BINARY_DIR}/UserDataExtension.toml" "$<TARGET_FILE_DIR:UserDataExtension>/UserDataExtension.toml")
if(USER_DATA_EXTENSION_DEPLOY)
    if(NOT USER_DATA_EXTENSION_DEPLOY_ROOT)
        message(FATAL_ERROR "Set USER_DATA_EXTENSION_DEPLOY_ROOT explicitly to enable deployment")
    endif()
    add_custom_command(TARGET UserDataExtension POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${USER_DATA_EXTENSION_DEPLOY_ROOT}/reframework/plugins"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:UserDataExtension>" "${USER_DATA_EXTENSION_DEPLOY_ROOT}/reframework/plugins/UserDataExtension.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_BINARY_DIR}/UserDataExtension.toml" "${USER_DATA_EXTENSION_DEPLOY_ROOT}/reframework/plugins/UserDataExtension.toml")
endif()

add_executable(UserDataRegistrationTests EXCLUDE_FROM_ALL UserDataRegistrationTests.cpp ${USER_DATA_EXTENSION_SOURCES})
target_include_directories(UserDataRegistrationTests PRIVATE ${CMAKE_SOURCE_DIR}/plugins/ClassInjector/include)
target_compile_definitions(UserDataRegistrationTests PRIVATE NOMINMAX USER_DATA_EXTENSION_STATIC)
target_compile_options(UserDataRegistrationTests PRIVATE /utf-8)
target_link_libraries(UserDataRegistrationTests PRIVATE common_lib nlohmann_json::nlohmann_json)

# 只显式构建/部署的零类型实机探针，复用生产执行器和离线检查，不进入生产自动部署。
add_library(UserDataSmallValueProbe SHARED EXCLUDE_FROM_ALL UserDataSmallValueProbe.cpp ${USER_DATA_EXTENSION_SOURCES})
target_include_directories(UserDataSmallValueProbe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/plugins/ClassInjector/include ${CMAKE_SOURCE_DIR}/common/include/sol2/single/include)
target_compile_definitions(UserDataSmallValueProbe PRIVATE NOMINMAX USER_DATA_EXTENSION_STATIC)
target_compile_options(UserDataSmallValueProbe PRIVATE /utf-8)
target_link_libraries(UserDataSmallValueProbe PRIVATE common_lib nlohmann_json::nlohmann_json)
