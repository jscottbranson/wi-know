idf_component_register(SRCS "src/core.cpp" "src/remote_id.cpp" INCLUDE_DIRS "include")
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
