add_executable(lychee_cli main.cc)
target_link_libraries(lychee_cli PRIVATE lychee)
target_compile_features(lychee_cli PRIVATE cxx_std_20)
