include_directories(
        ${PROJECT_SOURCE_DIR}/src/include
        ${CMAKE_BINARY_DIR}/src/include
        src/include
        third_party/snowball/libstemmer
        ${PROJECT_SOURCE_DIR}/third_party/cppjieba/include
        ${PROJECT_SOURCE_DIR}/third_party/cppjieba/deps/limonp/include
        ${PROJECT_SOURCE_DIR}/third_party/mecab/src)

add_subdirectory(src/catalog)
add_subdirectory(src/function)
add_subdirectory(src/main)
add_subdirectory(test)
add_subdirectory(src/index)
add_subdirectory(src/utils)

add_subdirectory(third_party/snowball)

# MeCab (Japanese tokenizer): static lib + mecab-dict-index tool + build-time
# ipadic dictionary compilation (prebuilt-ipadic/ bundled, offline).
add_subdirectory(${PROJECT_SOURCE_DIR}/third_party/mecab
        ${CMAKE_CURRENT_BINARY_DIR}/mecab)

build_extension_lib(${BUILD_STATIC_EXTENSION} "fts")

target_link_libraries(lbug_${EXTENSION_LIB_NAME}_extension
        PRIVATE
        snowball
        re2
        cppjieba
        mecab)

# fts_utils.cpp / tokenize.cpp include mecab.h, which requires DLL_EXPORT to
# declare the MeCab API without __declspec(dllimport) on Windows.
target_compile_definitions(lbug_fts_utils PRIVATE DLL_EXPORT)
target_compile_definitions(lbug_fts_function PRIVATE DLL_EXPORT)

# dict_dir_resolver.cpp uses dladdr() on non-Windows platforms to locate the
# extension module at runtime.
if(UNIX)
    target_link_libraries(lbug_${EXTENSION_LIB_NAME}_extension PRIVATE ${CMAKE_DL_LIBS})
endif()

# Copy Jieba dictionaries next to the built extension so runtime can find a default path
add_custom_command(TARGET lbug_${EXTENSION_LIB_NAME}_extension POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${PROJECT_SOURCE_DIR}/third_party/cppjieba/dict
        ${PROJECT_SOURCE_DIR}/extension/fts/build/dict)

# Copy the compiled ipadic dictionary (UTF-8) next to the built extension so
# the default mecab_dict_dir resolves at runtime.
add_custom_command(TARGET lbug_${EXTENSION_LIB_NAME}_extension POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_BINARY_DIR}/mecab-dict/ipadic
        ${PROJECT_SOURCE_DIR}/extension/fts/build/dict-ipadic)
add_dependencies(lbug_${EXTENSION_LIB_NAME}_extension mecab-ipadic-dict)
