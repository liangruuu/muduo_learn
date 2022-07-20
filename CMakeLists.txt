cmake_minimum_required(VERSION 2.5)
project(mymuduo_withnotes)

# mymuduo最终编程成so动态库，设置动态库的路径，放在根目录下的lib文件夹下
set(LIBRARY_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/lib)
# 设置调试信息 以及 启动c++11标准
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -std=c++11")

# 定义参与编译的源代码文件，"."表示当前目录
# 把当前目录下的源文件的名字组合起来放入SRC_LIST变量里
aux_source_directory(. SRC_LIST)
# 编译生成动态库mymuduo_withnotes
# 把所有参与编译的源文件都编译成一个名字叫mymuduo_withnotes的动态库
add_library(mymuduo_withnotes SHARED ${SRC_LIST})