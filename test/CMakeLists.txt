file(GLOB sources *.cpp)
add_executable(pstsdk_test ${sources})

if(Iconv_FOUND)
  target_link_libraries(pstsdk_test PUBLIC Iconv::Iconv)
endif()

add_test(NAME pstsdk_test COMMAND pstsdk_test WORKING_DIRECTORY ../test)
set_tests_properties(pstsdk_test PROPERTIES TIMEOUT 900)
