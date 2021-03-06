add_subdirectory(libfptu/src)
add_subdirectory(libfptu/test)

add_library(t1ha STATIC
   t1ha/src/t1ha0.c
   t1ha/src/t1ha1.c
)

if (CC_HAS_ARCH_NATIVE)
   target_compile_options(t1ha PRIVATE "-march=native")
endif()

###

if (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    message (WARNING "Sorry, but Windows MUST DIE, so only a stub will be provided.")
else()

add_library(mdbx STATIC
   libmdbx/mdbx.h
   libmdbx/mdbx.c
)

if(ENABLE_VALGRIND)
    set_target_properties(mdbx PROPERTIES COMPILE_FLAGS "-DNDEBUG -DENABLE_VALGRIND")
else()
    set_target_properties(mdbx PROPERTIES COMPILE_FLAGS "-DNDEBUG")
endif()

target_link_libraries(mdbx ${CMAKE_THREAD_LIBS_INIT})

add_executable(mdbx_chk libmdbx/mdbx_chk.c)
target_link_libraries(mdbx_chk mdbx)

add_executable(mdbx_stat libmdbx/mdbx_stat.c)
target_link_libraries(mdbx_stat mdbx)

add_executable(mdbx_copy libmdbx/mdbx_copy.c)
target_link_libraries(mdbx_copy mdbx)

add_executable(mdbx_dump libmdbx/mdbx_dump.c)
target_link_libraries(mdbx_dump mdbx)

add_executable(mdbx_load libmdbx/mdbx_load.c)
target_link_libraries(mdbx_load mdbx)

endif()
