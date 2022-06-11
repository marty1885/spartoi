add_executable(spartan_client client/spartan_client.cpp)
target_link_libraries(spartan_client PRIVATE spartoi)

add_executable(spartan_server server/spartan_server.cpp)
target_link_libraries(spartan_server PRIVATE spartoi)
add_custom_command(
  TARGET spartan_server POST_BUILD
  COMMAND ${CMAKE_COMMAND}
          -E
          copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/server/files
          $<TARGET_FILE_DIR:spartan_server>)
add_custom_command(
  TARGET spartan_server POST_BUILD
  COMMAND ${CMAKE_COMMAND}
          -E
	  copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/server/drogon.config.json
          $<TARGET_FILE_DIR:spartan_server>)
drogon_create_views(spartan_server ${CMAKE_CURRENT_SOURCE_DIR}/server/templates/
  ${CMAKE_CURRENT_BINARY_DIR})