cmake_minimum_required(VERSION 3.15)
project(lvm VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(LVM_NO_COMPUTED_GOTO "use switch dispatch even where computed goto exists" OFF)

if(MSVC)
  add_compile_options(/W3 /D_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra)
endif()

add_library(lvm STATIC
  src/util.c
  src/loader.c
  src/verify.c
  src/vm.c
  src/object.c
  src/gc.c
  src/container.c
  src/closure.c
  src/asm.c
  src/disasm.c
  src/host_std.c)
target_include_directories(lvm PUBLIC include PRIVATE src)
if(LVM_NO_COMPUTED_GOTO)
  target_compile_definitions(lvm PRIVATE LVM_NO_COMPUTED_GOTO)
endif()
if(NOT WIN32)
  target_link_libraries(lvm PUBLIC m)
endif()

add_executable(lvmi tools/lvmi.c)
target_link_libraries(lvmi PRIVATE lvm)

add_executable(lvma tools/lvma.c)
target_link_libraries(lvma PRIVATE lvm)

add_executable(lvmdis tools/lvmdis.c)
target_link_libraries(lvmdis PRIVATE lvm)

enable_testing()
add_executable(test_vm tests/test_vm.c)
target_link_libraries(test_vm PRIVATE lvm)
add_test(NAME vm COMMAND test_vm)

add_executable(test_asm tests/test_asm.c)
target_link_libraries(test_asm PRIVATE lvm)
file(GLOB LVM_EXAMPLES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/examples/*.lvma)
add_test(NAME asm COMMAND test_asm ${LVM_EXAMPLES})

install(TARGETS lvmi lvma lvmdis lvm RUNTIME DESTINATION bin ARCHIVE DESTINATION lib)
install(FILES include/lvm.h include/lvm_value.h include/lvm_opcode.h DESTINATION include)
install(FILES README.md docs/isa.md docs/format.md docs/asm.md DESTINATION share/doc/lvm)
install(DIRECTORY examples DESTINATION share/doc/lvm)
