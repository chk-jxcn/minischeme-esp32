# LittleFS (littlefs/, BSD-3-Clause, littlefs-project v2.11.3, unmodified)
# mounted into the ESP-IDF VFS by lfsvfs.c
idf_component_register(
    SRCS "lfsvfs.c" "littlefs/lfs.c" "littlefs/lfs_util.c"
    INCLUDE_DIRS "include" "littlefs"
    REQUIRES vfs esp_partition freertos log
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LFS_NO_DEBUG LFS_NO_WARN)
