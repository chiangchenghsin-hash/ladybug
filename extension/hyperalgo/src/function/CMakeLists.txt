add_library(lbug_hyperalgo_function
        OBJECT
        hyper_project.cpp
        hyper_kitting_bool.cpp
        hyperalgo_functions.cpp
        )

set(HYPERALGO_EXTENSION_OBJECT_FILES
        ${HYPERALGO_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_hyperalgo_function>
        PARENT_SCOPE)
