cmake_minimum_required(VERSION 2.8.3)
project(riptide_hardware)

find_package(catkin REQUIRED
    COMPONENTS
    roslint
    roscpp
    riptide_msgs
    geometry_msgs
    imu_3dm_gx4
    pointgrey_camera_driver
    message_filters
    message_generation
)

add_service_files(FILES Pneumatics.srv)

generate_messages()

catkin_package(INCLUDE_DIRS include)

include_directories(include ${catkin_INCLUDE_DIRS})

roslint_cpp()

# include boost
find_package(Boost REQUIRED)
include_directories(${catkin_INCLUDE_DIRS} ${Boost_INCLUDE_DIR})

add_executable(imu_processor src/imu_processor.cpp)
target_link_libraries(imu_processor ${catkin_LIBRARIES})

add_executable(imu_logger src/imu_logger.cpp)
target_link_libraries(imu_logger ${catkin_LIBRARIES})
add_dependencies(imu_logger ${catkin_EXPORTED_TARGETS})

add_dependencies(imu_processor riptide_msgs_gencpp)
add_dependencies(imu_processor ${catkin_EXPORTED_TARGETS})

add_executable(depth_processor src/depth_processor.cpp)
target_link_libraries(depth_processor ${catkin_LIBRARIES})
add_dependencies(depth_processor riptide_msgs_gencpp)
