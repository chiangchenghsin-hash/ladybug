add_library(lbug_fts_utils
        OBJECT
        dict_dir_resolver.cpp
        fts_utils.cpp)

set(FTS_EXTENSION_OBJECT_FILES
        ${FTS_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_fts_utils>
        PARENT_SCOPE)
