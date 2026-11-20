# ESP-IDF 顶层构建脚本：五行样板，顺序不能动
cmake_minimum_required(VERSION 3.16)

# 组件管理器（idf-component-manager）默认会“检查新版本”，这一步会去解析
# esp_hosted 3.1.0 的清单规则，里面 $CONFIG{...} == 1 的写法与 IDF 5.5.3
# 自带的组件管理器件不兼容，报 "Missing required kconfig option after retry"。
# 关掉新版本检查后，只按 dependencies.lock 里锁定的版本走（本项目锁 2.12.9）。
set(ENV{IDF_COMPONENT_CHECK_NEW_VERSION} "0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4_lab)
