# MyWebServer CMake 学习记录

这份记录以本项目根目录的 `CMakeLists.txt` 为例。目标不是背完所有命令，而是能够回答三个问题：

1. CMake 如何把源码变成可执行程序？
2. MySQL、Hiredis、libuv 和 pthread 是如何加入项目的？
3. 如何切换 Debug/Release、运行测试并排查构建错误？

## 1. 先建立整体认识

CMake 不是编译器，也不是 `make`。它读取 `CMakeLists.txt`，检查编译器和依赖，然后生成 Ninja 或 Makefile 等底层构建文件。

一次构建通常分为三步：

```text
CMakeLists.txt
    ↓ cmake -S . -B build（配置、生成）
build/Makefile 或 build.ninja
    ↓ cmake --build build（实际编译、链接）
build/my_web_server
```

本项目面向 Linux，依赖 `pthread`、MySQL Client、Hiredis 和 libuv，因此应在 WSL 或 Linux 中构建。

常用命令：

```bash
# 在项目根目录执行，生成 Release 构建目录
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 并行编译；--build 屏蔽了 make 和 ninja 的差异
cmake --build build -j

# 开启测试、重新配置并编译
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j

# 执行 CTest 中登记的测试
ctest --test-dir build --output-on-failure

# 不需要测试的生产镜像构建
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build -j
```

`-S .` 表示源码目录是当前目录；`-B build` 表示所有生成文件放入 `build/`。这种“源码外构建”不会把中间文件混入源码目录。

## 2. CMake 的基础语法

### 2.1 命令和参数

```cmake
命令名(参数1 参数2 参数3)
```

CMake 命令名不区分大小写，但项目通常统一使用小写。换行一般只用于排版：

```cmake
add_executable(my_web_server
    src/main.cpp
    src/server/WebServer.cpp
)
```

### 2.2 变量

```cmake
set(SERVER_NAME my_web_server)
message(STATUS "target = ${SERVER_NAME}")
```

- `set(NAME value)`：设置变量。
- `${NAME}`：读取变量的值。
- `message(STATUS ...)`：在配置阶段打印信息。
- CMake 普通变量本质上是字符串；列表本质上是以分号分隔的字符串。

```cmake
set(SOURCES src/main.cpp src/server/WebServer.cpp)
add_executable(my_web_server ${SOURCES})
```

### 2.3 条件判断

```cmake
if(MYSQLCLIENT_FOUND)
    message(STATUS "MySQL support enabled")
else()
    message(WARNING "MySQL support disabled")
endif()
```

常见判断：

```cmake
if(NOT BUILD_TESTING)
if(TARGET my_web_server)
if(EXISTS "${PROJECT_SOURCE_DIR}/config.json")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
```

注意：比较字符串时使用 `STREQUAL`，数值比较使用 `EQUAL`、`LESS`、`GREATER`。

### 2.4 列表操作

```cmake
list(APPEND SOURCES src/utils/Metrics.cpp)
list(REMOVE_ITEM SOURCES src/old.cpp)
list(FILTER SOURCES EXCLUDE REGEX ".*/old/.*")
```

本项目使用 `list(FILTER ... EXCLUDE REGEX ...)`，把旧的同步 Session 实现从服务器源码中排除。

### 2.5 注释

```cmake
# 单行注释
```

## 3. 逐段理解本项目的 CMakeLists.txt

### 3.1 指定最低版本

```cmake
cmake_minimum_required(VERSION 3.10)
```

它表示项目至少需要 CMake 3.10，也会确定一组 CMake 策略的默认行为。应把它放在文件开头。

### 3.2 声明项目

```cmake
project(MyWebServer)
```

这会创建一些内置变量：

- `${PROJECT_NAME}`：`MyWebServer`
- `${PROJECT_SOURCE_DIR}`：顶层源码目录
- `${PROJECT_BINARY_DIR}`：顶层构建目录，通常是 `build/`

更完整的写法是：

```cmake
project(MyWebServer VERSION 1.0.0 LANGUAGES CXX)
```

### 3.3 设置 C++ 标准

```cmake
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
```

- 要求使用 C++17。
- `REQUIRED ON` 表示编译器不支持 C++17 时直接报错，而不是悄悄退回旧标准。

更现代、粒度更小的 target 写法是：

```cmake
target_compile_features(my_web_server PRIVATE cxx_std_17)
```

### 3.4 默认使用 Release

```cmake
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
```

- `CMAKE_BUILD_TYPE`：单配置生成器当前使用的配置，例如 `Debug` 或 `Release`。
- `CMAKE_CONFIGURATION_TYPES`：Visual Studio 等多配置生成器支持的配置列表。
- `CACHE`：把变量写进 `build/CMakeCache.txt`，以后重新配置还能保留。
- `FORCE`：强制覆盖缓存值。

外部命令显式传入的值优先满足前面的判断：

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
```

常见配置：

- `Debug`：有调试符号、优化低，适合 GDB。
- `Release`：高优化，通常关闭断言，适合部署和压测。
- `RelWithDebInfo`：有优化也有调试符号，适合分析线上性能问题。

### 3.5 添加头文件搜索目录

```cmake
include_directories(${PROJECT_SOURCE_DIR}/include)
```

效果类似给编译器添加：

```text
-I/path/to/MyWebServer/include
```

因此源码可以写：

```cpp
#include "server/WebServer.h"
```

`include_directories` 会影响当前目录和子目录，范围比较大。现代 CMake 更推荐：

```cmake
target_include_directories(my_web_server PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
```

### 3.6 使用 pkg-config 查找第三方库

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQLCLIENT QUIET mysqlclient)
pkg_check_modules(HIREDIS REQUIRED hiredis)
pkg_check_modules(LIBUV REQUIRED libuv)
```

`find_package(PkgConfig REQUIRED)` 先找到 `pkg-config` 工具。随后 `pkg_check_modules` 查询系统中安装的 `.pc` 文件，例如：

```bash
pkg-config --cflags --libs hiredis
pkg-config --cflags --libs libuv
pkg-config --cflags --libs mysqlclient
```

查询后会生成变量：

- `${HIREDIS_FOUND}`：是否找到。
- `${HIREDIS_INCLUDE_DIRS}`：头文件路径。
- `${HIREDIS_CFLAGS_OTHER}`：其他编译参数。
- `${HIREDIS_LDFLAGS}`：库路径、库名和其他链接参数。

`REQUIRED` 表示找不到就停止配置；`QUIET` 表示安静查询，不因找不到而直接失败。本项目中 Hiredis 和 libuv 是必须依赖，而 MySQL Client 被设计成可选依赖。

### 3.7 收集源文件

```cmake
file(GLOB_RECURSE SRC_FILES
    ${PROJECT_SOURCE_DIR}/src/*.cpp
)
```

它递归寻找 `src/` 下的所有 `.cpp`，把结果存入 `SRC_FILES`。

优点是添加源文件时少写一行；缺点是文件新增后，构建系统不一定自动知道需要重新运行 CMake。更稳妥的两种方式是显式列文件，或者使用：

```cmake
file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/src/*.cpp
)
```

大型、长期维护的项目通常更倾向显式列出源文件，因为代码变更在 Git diff 中更清楚。

### 3.8 排除旧实现

```cmake
list(FILTER SRC_FILES EXCLUDE REGEX
    ".*/src/auth/SessionManager\\.cpp$"
)
```

项目同时保留了同步 `SessionManager.cpp` 和异步 `AsyncSessionManager.cpp`。如果两个实现包含重复符号，全部编入服务器可能链接失败；即使不重复，也不应该把未使用的旧实现放入生产目标，因此这里过滤掉同步版本。

正则中的 `\\.` 最终用于匹配文件名里的点号，而不是“任意字符”。

### 3.9 创建可执行目标

```cmake
add_executable(my_web_server
    ${SRC_FILES}
)
```

`my_web_server` 是一个 target。之后的头文件目录、编译宏、编译选项和链接库都应尽量挂到这个 target 上。

target 可以是：

```cmake
add_executable(app main.cpp)
add_library(core STATIC core.cpp)
add_library(plugin SHARED plugin.cpp)
add_library(headers INTERFACE)
```

### 3.10 设置编译和链接参数

```cmake
target_include_directories(my_web_server PRIVATE
    ${HIREDIS_INCLUDE_DIRS}
    ${LIBUV_INCLUDE_DIRS}
)

target_compile_options(my_web_server PRIVATE
    ${HIREDIS_CFLAGS_OTHER}
    ${LIBUV_CFLAGS_OTHER}
)

target_link_libraries(my_web_server
    pthread
    ${HIREDIS_LDFLAGS}
    ${LIBUV_LDFLAGS}
)
```

三个阶段不能混淆：

- `target_include_directories`：解决“找不到 `.h`”。
- `target_compile_options`：设置编译参数，例如警告或优化开关。
- `target_link_libraries`：解决“找不到函数实现/undefined reference”。

`pthread` 是 POSIX 线程库。它也说明当前构建文件主要面向 Linux，而不是原生 Windows。

更可移植的写法通常是：

```cmake
find_package(Threads REQUIRED)
target_link_libraries(my_web_server PRIVATE Threads::Threads)
```

### 3.11 PRIVATE、PUBLIC、INTERFACE

这三个词描述“依赖是否向使用者传播”：

| 关键字 | 当前 target 使用 | 使用当前 target 的下游使用 |
|---|---:|---:|
| `PRIVATE` | 是 | 否 |
| `PUBLIC` | 是 | 是 |
| `INTERFACE` | 否 | 是 |

例如：

```cmake
add_library(core STATIC core.cpp)
target_include_directories(core PUBLIC include)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE core)
```

因为 `core` 的公开头文件也会被 `app` 引用，所以使用 `PUBLIC include`。Hiredis 只在 `my_web_server` 内部使用时，应标记为 `PRIVATE`。

### 3.12 条件启用 MySQL

```cmake
if(MYSQLCLIENT_FOUND)
    target_compile_definitions(my_web_server PRIVATE MYWEBSERVER_WITH_MYSQL)
    target_include_directories(my_web_server PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS})
    target_compile_options(my_web_server PRIVATE ${MYSQLCLIENT_CFLAGS_OTHER})
    target_link_libraries(my_web_server ${MYSQLCLIENT_LDFLAGS})
    message(STATUS "MySQL support enabled")
else()
    message(WARNING "mysqlclient was not found...")
endif()
```

`target_compile_definitions` 会添加编译宏，效果类似：

```text
-DMYWEBSERVER_WITH_MYSQL
```

C++ 中可以据此条件编译：

```cpp
#ifdef MYWEBSERVER_WITH_MYSQL
// MySQL 实现
#else
// 数据库不可用时的降级实现
#endif
```

这叫“可选特性”：有 mysqlclient 就编译数据库能力，没有也允许服务器主体构建。

### 3.13 注册测试

```cmake
include(CTest)

if(BUILD_TESTING)
    add_executable(thread_pool_capacity_test
        tests/thread_pool_capacity_test.cpp
    )
    target_link_libraries(thread_pool_capacity_test pthread)
    add_test(
        NAME thread_pool_capacity_test
        COMMAND thread_pool_capacity_test
    )
endif()
```

- `include(CTest)`：引入 CTest，并提供 `BUILD_TESTING` 选项，默认通常为 `ON`。
- `add_executable`：测试源码也必须先编译成程序。
- `add_test`：把程序登记给 CTest；它本身不负责编译。

本项目还设置了：

```cmake
set_tests_properties(async_session_manager_test
    PROPERTIES SKIP_RETURN_CODE 77
)
```

当测试返回 77 时，CTest 将它记为“跳过”而不是“失败”。这适合依赖外部 Redis、而当前环境没有准备 Redis 的情况。

## 4. 编译错误应该怎么判断

### 配置阶段错误

表现：执行 `cmake -S . -B build` 就失败。

常见原因：

- 找不到编译器。
- 找不到 `pkg-config`。
- 找不到 Hiredis 或 libuv。
- CMake 语法或路径写错。

检查依赖：

```bash
pkg-config --modversion hiredis
pkg-config --modversion libuv
pkg-config --modversion mysqlclient
```

### 编译阶段错误

表现：出现 `fatal error: xxx.h: No such file or directory`、语法错误等。

优先检查：

- `target_include_directories` 是否包含正确头文件目录。
- 头文件名和 `#include` 路径是否正确。
- C++ 标准是否满足代码要求。

### 链接阶段错误

表现：出现 `undefined reference to ...`。

说明声明已经找到，但函数实现没有被链接。优先检查：

- 实现所在 `.cpp` 是否加入 target。
- 依赖库是否加入 `target_link_libraries`。
- 库的链接顺序、版本和 ABI 是否匹配。

### 运行阶段错误

表现：程序构建成功，但启动时报 `cannot open shared object file`。

这是动态库加载问题，不再是头文件问题。可以检查：

```bash
ldd build/my_web_server
```

## 5. 常用排查命令

```bash
# 显示实际执行的编译、链接命令
cmake --build build --verbose

# 查看缓存变量
cmake -LAH -N build

# 删除构建目录后进行干净构建
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

# 只运行一个测试
ctest --test-dir build -R heap_timer_test --output-on-failure
```

删除 `build/` 只会删除生成物，不会删除源码。但执行前仍应确认当前目录和目标路径正确。

## 6. 本项目可以进一步采用的现代 CMake 写法

当前文件可以正常工作，但学习时应知道以下改进方向。

### 6.1 使用 imported target

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)
pkg_check_modules(LIBUV REQUIRED IMPORTED_TARGET libuv)

target_link_libraries(my_web_server PRIVATE
    PkgConfig::HIREDIS
    PkgConfig::LIBUV
)
```

imported target 会把头文件目录、编译参数和链接参数打包在一个逻辑目标里，减少手动搬运 `${..._INCLUDE_DIRS}`、`${..._CFLAGS_OTHER}` 和 `${..._LDFLAGS}`。

### 6.2 抽取核心库供服务器和测试复用

```cmake
add_library(mywebserver_core STATIC
    src/timer/HeapTimer.cpp
    src/utils/Metrics.cpp
)

target_include_directories(mywebserver_core PUBLIC include)

add_executable(my_web_server src/main.cpp)
target_link_libraries(my_web_server PRIVATE mywebserver_core)

add_executable(heap_timer_test tests/heap_timer_test.cpp)
target_link_libraries(heap_timer_test PRIVATE mywebserver_core)
```

这样测试不需要再次手动列出同一批实现文件，公共编译选项也只设置一次。

### 6.3 添加统一警告

```cmake
target_compile_options(my_web_server PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
```

生产项目通常会根据 GCC/Clang/MSVC 分别设置，避免把某个编译器专属参数传给其他编译器。

## 7. 面试常问问题

### CMake 和 Make 有什么区别？

CMake 是构建系统生成器；Make 是实际根据 Makefile 判断依赖并执行编译命令的工具。`cmake --build` 可以统一调用 Make、Ninja 或 Visual Studio 等底层构建工具。

### 为什么推荐源码外构建？

生成文件、对象文件和缓存全部留在 `build/`，源码目录干净；Debug、Release 也能分别使用不同构建目录。

### 头文件已经找到了，为什么还会 undefined reference？

头文件只提供声明。链接器还必须从项目 `.o` 文件或第三方库中找到实现，因此要检查源文件和 `target_link_libraries`。

### `PRIVATE`、`PUBLIC`、`INTERFACE` 怎么选？

只在实现内部使用选 `PRIVATE`；当前库和下游都要使用选 `PUBLIC`；当前 target 自己不使用、只要求下游使用选 `INTERFACE`。

### 为什么 `file(GLOB_RECURSE)` 有争议？

新增文件可能不会自动触发 CMake 重新配置，而且源文件变化不直接出现在 `CMakeLists.txt` 的 Git diff 中。小项目使用方便，大项目常显式列文件或增加 `CONFIGURE_DEPENDS`。

### `find_package` 成功是否等于链接成功？

不一定。找到包以后，还要把包提供的 imported target 或 include/link 变量正确绑定到自己的 target。

### 为什么测试能编译但 `ctest` 找不到？

因为只有 `add_executable`，没有使用 `add_test` 注册；或者配置时关闭了 `BUILD_TESTING`。

## 8. 建议的学习顺序

1. 熟练执行 `cmake -S`、`-B`、`--build` 和 `ctest`。
2. 掌握 `add_executable`、`add_library`、`target_include_directories`、`target_link_libraries`。
3. 理解 target 和 `PRIVATE/PUBLIC/INTERFACE`，这是现代 CMake 的核心。
4. 学会 `find_package`、pkg-config 和 imported target。
5. 再学习 `option`、`install`、子目录、函数、工具链文件和交叉编译。

判断自己是否真的理解，可以尝试完成三个练习：

1. 新增一个只包含 `main()` 的可执行 target，并成功编译。
2. 把 `HeapTimer.cpp` 抽成静态库，让服务器和测试共同链接它。
3. 分别生成 Debug 和 Release 构建目录，用 `--verbose` 对比两者的编译参数。
