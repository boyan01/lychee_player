add_executable(lychee_cli main.cc)
target_link_libraries(lychee_cli PRIVATE lychee)
if(APPLE)
  # Describes LYCHEE_FRAME_CVPIXELBUFFER_NATIVE frames.
  target_link_libraries(lychee_cli PRIVATE "-framework CoreVideo")
elseif(WIN32)
  # --d3d11 reads LYCHEE_FRAME_D3D11_TEXTURE frames back.
  target_link_libraries(lychee_cli PRIVATE d3d11 dxgi)
  target_compile_definitions(lychee_cli PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
target_compile_features(lychee_cli PRIVATE cxx_std_20)
target_include_directories(lychee_cli PRIVATE ${PROJECT_SOURCE_DIR}/core/src/api)
