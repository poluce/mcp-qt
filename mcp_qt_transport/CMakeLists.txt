set(CMAKE_AUTOMOC ON)

add_library(mcp_qt_transport STATIC
    include/mcp_qt_transport/QtHttpSseTransport.h
    include/mcp_qt_transport/QtProcessStdioTransport.h
    include/mcp_qt_transport/QtStatelessHttpTransport.h
    src/QtHttpSseTransport.cpp
    src/QtProcessStdioTransport.cpp
    src/QtStatelessHttpTransport.cpp
    src/QtHttpSseWorker.h
    src/QtHttpSseWorker.cpp
    src/QtSseParser.h
    src/QtSseParser.cpp
)

target_include_directories(mcp_qt_transport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/mcp_core/include
)

target_link_libraries(mcp_qt_transport PUBLIC
    mcp_core
    Qt6::Core
    Qt6::Network
)

target_compile_features(mcp_qt_transport PUBLIC cxx_std_17)
target_compile_options(mcp_qt_transport PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

add_subdirectory(${CMAKE_SOURCE_DIR}/tests_qt ${CMAKE_BINARY_DIR}/tests_qt)
