add_library(hyperalgo_extension_main
        OBJECT
        hyperalgo_extension.cpp
        )

set(HYPERALGO_EXTENSION_OBJECT_FILES
        ${HYPERALGO_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:hyperalgo_extension_main>
        PARENT_SCOPE)
