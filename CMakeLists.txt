cmake_minimum_required(VERSION 3.15)
project(liburing_examples C)

set(CMAKE_C_STANDARD 99)

add_executable(probe probe.c)
target_link_libraries(probe uring)

add_executable(link link.c)
target_link_libraries(link uring)

add_executable(fixed_buffers fixed_buffers.c)
target_link_libraries(fixed_buffers uring)

add_executable(sq_poll sq_poll.c)
target_link_libraries(sq_poll uring)

add_executable(provide_buffers provide_buffers.c)
target_link_libraries(provide_buffers uring)

add_executable(eventfd eventfd.c)
target_link_libraries(eventfd uring pthread)

add_executable(cat_io_uring cat_io_uring.c)

add_executable(cat_liburing cat_liburing.c)
target_link_libraries(cat_liburing uring)

add_executable(webserver_liburing webserver_liburing.c)
target_link_libraries(webserver_liburing uring)

add_executable(http_client examples/http_client.c)
target_link_libraries(http_client uring)

# End-to-end tests. tests/smoke.sh starts the server on port 8000 and drives it
# over a real socket, so it needs bash and a free port -- run it directly rather
# than relying on this, but wire it up so `ctest` picks it up too.
enable_testing()
find_program(BASH_EXECUTABLE bash)
if(BASH_EXECUTABLE)
    add_test(NAME smoke
             COMMAND ${BASH_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.sh)
    set_tests_properties(smoke PROPERTIES
        ENVIRONMENT "SERVER_BIN=$<TARGET_FILE:webserver_liburing>")
endif()