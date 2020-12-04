set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(EPOLL_SHIM_SRC
    epoll_shim_ctx.c
    epoll.c
    epollfd_ctx.c
    timerfd.c
    timerfd_ctx.c
    signalfd.c
    signalfd_ctx.c
    eventfd.c
    eventfd_ctx.c)

if(APPLE)
  add_library(epoll-shim STATIC ${EPOLL_SHIM_SRC})
else()
  add_library(epoll-shim ${EPOLL_SHIM_SRC})
endif()

target_link_libraries(epoll-shim PRIVATE Threads::Threads)
target_include_directories(
  epoll-shim
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

if(NOT APPLE)
  target_link_options(epoll-shim PRIVATE
                      "LINKER:--version-script=${PROJECT_SOURCE_DIR}/Version.map")
endif()
set_target_properties(epoll-shim PROPERTIES SOVERSION 0)
