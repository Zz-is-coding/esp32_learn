# idf_component_register(SRCS "main.c"
#                     PRIV_REQUIRES spi_flash
#                     INCLUDE_DIRS ".")
set(src_dirs        .
                    src
                    ai)

set(include_dirs    include
                    ai
                    .)
idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs})
