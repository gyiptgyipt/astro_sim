#Copyright(c) 2017, United States Government, as represented by the
#Administrator of the National Aeronautics and Space Administration.
#
#All rights reserved.
#
#The Astrobee platform is licensed under the Apache License, Version 2.0
#(the "License"); you may not use this file except in compliance with the
#License.You may obtain a copy of the License at
#
#http:  // www.apache.org/licenses/LICENSE-2.0
#
#Unless required by applicable law or agreed to in writing, software
#distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
#WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.See the
#License for the specific language governing permissions and limitations
#under the License.

cmake_minimum_required(VERSION 3.0)
project(imu_bias_tester)

if (USE_ROS)
  ## Compile as C++14, supported in ROS Kinetic and newer
  add_compile_options(-std=c++14)
  add_definitions(-DROS1)

  ## Find catkin macros and libraries
  find_package(catkin2 REQUIRED COMPONENTS
    roscpp
    nodelet
    ff_util
    ff_msgs
    config_reader
    imu_integration
  )

  catkin_package(
    LIBRARIES ${PROJECT_NAME} ${GLOG_LIBRARIES} ${GTSAM_LIBRARIES}
    INCLUDE_DIRS include ${GTSAM_INCLUDE_DIR} ${GLOG_INCLUDE_DIRS} 
    CATKIN_DEPENDS roscpp ff_util ff_msgs config_reader imu_integration
  )
   
  ###########
  ## Build ##
  ###########
  # Specify additional locations of header files
  include_directories(
    include
    ${catkin_INCLUDE_DIRS}
    ${EIGEN3_INCLUDE_DIRS}
  )

    # Declare C++ libraries
    add_library(${PROJECT_NAME}
      src/imu_bias_tester.cc
      src/imu_bias_tester_nodelet.cc
      src/imu_bias_tester_wrapper.cc
    )
    add_dependencies(${PROJECT_NAME} ${catkin_EXPORTED_TARGETS})
    target_link_libraries(${PROJECT_NAME} ${catkin_LIBRARIES})


  #############
  ## Install ##
  #############

  # Mark libraries for installation
  install(TARGETS ${PROJECT_NAME}
    ARCHIVE DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
    LIBRARY DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
    RUNTIME DESTINATION ${CATKIN_GLOBAL_BIN_DESTINATION}
  )

  # Mark nodelet_plugin for installation
  install(FILES nodelet_plugins.xml
          DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION}
  )

  # Mark cpp header files for installation
  install(DIRECTORY include/${PROJECT_NAME}/
    DESTINATION ${CATKIN_PACKAGE_INCLUDE_DESTINATION}
    FILES_MATCHING PATTERN "*.h"
    PATTERN ".svn" EXCLUDE
  )

  # Mark launch files for installation
  install(DIRECTORY launch/
    DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION}/launch
    PATTERN ".svn" EXCLUDE)

else (USE_ROS)
  find_package(catkin REQUIRED COMPONENTS)
  catkin_package()
endif (USE_ROS)
