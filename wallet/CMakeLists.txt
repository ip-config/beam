set(TARGET_NAME beam-wallet)

add_definitions( -DSQLITE_HAS_CODEC )

set(WALLET_SRC
    common.cpp
    wallet.h
    wallet.cpp
    wallet_transaction.cpp
    wallet_network.cpp
    wallet_db.cpp
    wallet_client.h
    wallet_client.cpp
    swap_transaction.cpp
    secstring2.cpp
    unittests/util.cpp
)

add_library(wallet STATIC ${WALLET_SRC})
add_dependencies(wallet core p2p sqlite)
target_link_libraries(wallet core p2p sqlite)

add_executable(${TARGET_NAME} cli.cpp)

target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external)

if(LINUX)
	target_link_libraries(${TARGET_NAME} -static-libstdc++ -static-libgcc)
endif()

configure_file("${PROJECT_SOURCE_DIR}/version.h.in" "${CMAKE_CURRENT_BINARY_DIR}/version.h")
target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

add_dependencies(${TARGET_NAME} wallet utility)
target_link_libraries(${TARGET_NAME} wallet utility)

add_library(wallet_api_proto STATIC api.cpp)
add_dependencies(wallet_api_proto wallet utility)
target_link_libraries(wallet_api_proto wallet utility)

add_executable(wallet-api api_cli.cpp)
target_include_directories(wallet-api PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
add_dependencies(wallet-api wallet utility wallet_api_proto)
target_link_libraries(wallet-api wallet utility wallet_api_proto)

if(LINUX)
    target_link_libraries(wallet-api -static-libstdc++ -static-libgcc)
endif()

add_subdirectory(unittests)

if (BEAM_SIGN_PACKAGE AND WIN32)
    add_custom_command(
        TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${SIGNTOOL_EXECUTABLE} sign ${SIGNTOOL_OPTIONS} "${CMAKE_CURRENT_BINARY_DIR}\\beam-wallet.exe"
    )
    add_custom_command(
        TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${SIGNTOOL_EXECUTABLE} sign ${SIGNTOOL_OPTIONS} "${CMAKE_CURRENT_BINARY_DIR}\\wallet-api.exe"
    )
endif()