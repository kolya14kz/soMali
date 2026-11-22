# CMakeLists.txt – сборка Vulkan‑wrapper .so для Android (NDK)

cmake_minimum_required(VERSION 3.22)
project(VkWrapper LANGUAGES CXX)

# Настройки NDK / Android
set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)
set(CMAKE_ANDROID_API 30)                     # минимум API 30 – поддержка Vulkan 1.1
set(CMAKE_ANDROID_STL_TYPE c++_static)        # статическая STL
set(CMAKE_ANDROID_NDK ${ANDROID_NDK_ROOT})    # переменная должна быть задана в окружении

#Флаги компиляции
add_compile_options(
    -std=c++20
    -Wall -Wextra -Werror
    -O3
    -march=armv8-a+simd
    -flto                    # Link‑time optimisation
    -fvisibility=hidden      # экспорт только нужных символов
)

# Описание библиотеки
add_library(VkWrapper SHARED src/wrapper.cpp)

target_include_directories(VkWrapper PRIVATE
    ${CMAKE_ANDROID_NDK}/sysroot/usr/include   # Vulkan headers из NDK
    src                                        # VMA заголовок рядом с кодом
)

# Препроцессорный макрос, который заставит VMA использовать наши vkGet*ProcAddr
target_compile_definitions(VkWrapper PRIVATE VMA_STATIC_VULKAN_FUNCTIONS)

target_link_libraries(VkWrapper
    android      # нужен для __android_log_print
    log
    dl
)

# Переименовываем так, как ожидает JSON‑файл
set_target_properties(VkWrapper PROPERTIES
    OUTPUT_NAME "libVkWrapper"
    PREFIX ""               # без префикса lib, иначе получим liblibVkWrapper.so
)

#Установка (можно скопировать в /data/local/tmp)
install(TARGETS VkWrapper
        DESTINATION ${CMAKE_CURRENT_BINARY_DIR})