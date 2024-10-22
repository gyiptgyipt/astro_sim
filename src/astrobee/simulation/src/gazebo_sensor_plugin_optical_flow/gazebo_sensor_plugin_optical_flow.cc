/* Copyright (c) 2017, United States Government, as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 *
 * All rights reserved.
 *
 * The Astrobee platform is licensed under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with the
 * License. You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

// Sensor plugin interface
#include <astrobee_gazebo/astrobee_gazebo.h>

// FSW includes
#include <config_reader/config_reader.h>

// General messages
// #include <sensor_msgs/PointCloud2.h>
// namespace sensor_msgs {
// typedef msg::PointCloud2 PointCloud2;
// }  // namespace sensor_msgs

// FSW messages
#include <ff_msgs/msg/camera_registration.hpp>
#include <ff_msgs/msg/feature2d_array.hpp>
#include <ff_msgs/msg/feature2d.hpp>
#include <ff_msgs/srv/set_bool.hpp>
namespace ff_msgs {
typedef msg::CameraRegistration CameraRegistration;
typedef msg::Feature2dArray Feature2dArray;
typedef msg::Feature2d Feature2d;
typedef srv::SetBool SetBool;
}  // namespace ff_msgs

// Camera model
#include <camera/camera_model.h>
#include <camera/camera_params.h>

// STL includes
#include <string>
#include <unordered_map>

namespace gazebo {
FF_DEFINE_LOGGER("gazebo_sensor_plugin_optical_flow");

class GazeboSensorPluginOpticalFlow : public FreeFlyerSensorPlugin {
 public:
  GazeboSensorPluginOpticalFlow()
      : FreeFlyerSensorPlugin("optical_flow_nodelet", "nav_cam", true), active_(true), id_(0) {}

  ~GazeboSensorPluginOpticalFlow() {}

 protected:
  // Called when plugin is loaded into gazebo
  void LoadCallback(NodeHandle& nh, sensors::SensorPtr sensor, sdf::ElementPtr sdf) {
    // Get a link to the parent sensor
    sensor_ = std::dynamic_pointer_cast<sensors::WideAngleCameraSensor>(sensor);
    if (!sensor_) {
      gzerr << "GazeboSensorPluginOpticalFlow requires a parent camera sensor.\n";
      return;
    }

    // Check that we have a mono camera
    if (sensor_->Camera()->ImageFormat() != "L8") FF_FATAL_STREAM("Camera format must be L8");

    // Build the nav cam Camera Model
    config_.AddFile("cameras.config");
    config_.AddFile("simulation/optical_flow.config");
    if (!config_.ReadFiles()) {
      FF_ERROR("Failed to read config files.");
      return;
    }

    if (!config_.GetReal("rate", &rate_))
      FF_FATAL("Could not read the rate parameter.");

    if (!config_.GetReal("delay_camera", &delay_camera_))
      FF_FATAL("Could not read the delay_camera parameter.");

    if (!config_.GetReal("delay_features", &delay_features_))
      FF_FATAL("Could not read the delay_features parameter.");

    if (!config_.GetReal("near_clip", &near_clip_))
      FF_FATAL("Could not read the near_clip parameter.");

    if (!config_.GetReal("far_clip", &far_clip_))
      FF_FATAL("Could not read the far_clip parameter.");

    if (!config_.GetUInt("num_features", &num_features_))
      FF_FATAL("Could not read the num_features parameter.");

    // Create a publisher for the registration messages
    pub_reg_ = FF_CREATE_PUBLISHER(nh, ff_msgs::CameraRegistration, TOPIC_LOCALIZATION_OF_REGISTRATION, 1);

    // Create a publisher for the feature messages
    pub_feat_ = FF_CREATE_PUBLISHER(nh, ff_msgs::Feature2dArray, TOPIC_LOCALIZATION_OF_FEATURES, 1);

    // Create a shape for collision testing
    GetWorld()->Physics()->InitForThread();
    shape_ = boost::dynamic_pointer_cast<physics::RayShape>(
      GetWorld()->Physics()->CreateShape("ray", physics::CollisionPtr()));

    // Only do this once
    msg_feat_.header.frame_id = std::string(FRAME_NAME_WORLD);
    msg_reg_.header.frame_id = std::string(FRAME_NAME_WORLD);
  }

  // Only send measurements when extrinsics are available
  void OnExtrinsicsReceived(NodeHandle& nh) {
    // Servide for enabling optical flow
    srv_enable_ = nh->create_service<ff_msgs::SetBool>(
      SERVICE_LOCALIZATION_OF_ENABLE,
      std::bind(&GazeboSensorPluginOpticalFlow::EnableService, this, std::placeholders::_1, std::placeholders::_2));

    // Timer triggers registration
    timer_registration_.createTimer(1.0 / rate_, std::bind(&GazeboSensorPluginOpticalFlow::SendRegistration, this), nh,
                                    false, true);

    // Timer triggers features
    timer_features_.createTimer(0.8 / rate_, std::bind(&GazeboSensorPluginOpticalFlow::SendFeatures, this), nh, true,
                                false);
  }

  // Enable or disable the feature timer
  bool EnableService(const std::shared_ptr<ff_msgs::srv::SetBool::Request> req,
                     std::shared_ptr<ff_msgs::srv::SetBool::Response> res) {
    active_ = req->enable;
    res->success = true;
    return true;
  }

  // Send a registration pulse
  void SendRegistration() {
    if (!active_) return;

    // Add a short delay between the features and new registration pulse
    timer_features_.stop();
    timer_features_.start();

    // Send the registration pulse
    msg_reg_.header.stamp = GetTimeNow();
    msg_reg_.camera_id++;
    pub_reg_->publish(msg_reg_);

    // Copy over the camera id to the feature message
    msg_feat_.camera_id = msg_reg_.camera_id;

    // Handle the transform for all sensor types
    Eigen::Affine3d wTb = (
        Eigen::Translation3d(
          GetModel()->WorldPose().Pos().X(),
          GetModel()->WorldPose().Pos().Y(),
          GetModel()->WorldPose().Pos().Z()) *
        Eigen::Quaterniond(
          GetModel()->WorldPose().Rot().W(),
          GetModel()->WorldPose().Rot().X(),
          GetModel()->WorldPose().Rot().Y(),
          GetModel()->WorldPose().Rot().Z()));

    Eigen::Affine3d bTs = (
        Eigen::Translation3d(
          sensor_->Pose().Pos().X(),
          sensor_->Pose().Pos().Y(),
          sensor_->Pose().Pos().Z()) *
        Eigen::Quaterniond(
          sensor_->Pose().Rot().W(),
          sensor_->Pose().Rot().X(),
          sensor_->Pose().Rot().Y(),
          sensor_->Pose().Rot().Z()));
    Eigen::Affine3d wTs = wTb * bTs;

    msg_feat_.header.stamp = GetTimeNow();

    // Initialize the camera paremeters
    static camera::CameraParameters cam_params(&config_, "nav_cam");
    static camera::CameraModel camera(Eigen::Vector3d(0, 0, 0), Eigen::Matrix3d::Identity(), cam_params);

    // Sample some features from the map. If we hav an issue sampling the map
    // then avoid sending features, at least for this round.
    if (!BuildAndSampleMap(camera, wTs, msg_feat_)) timer_features_.stop();
  }

  // Send a registration pulse
  void SendFeatures() {
    if (!active_) return;
    pub_feat_->publish(msg_feat_);
  }

 protected:
  // Sample a map, which is built online
  bool BuildAndSampleMap(camera::CameraModel const& camera, Eigen::Affine3d const& wTs, ff_msgs::Feature2dArray& msg) {
    // Make sure we have the correct number of elements
    map_.resize(num_features_);

    // Clear the features
    msg.feature_array.resize(num_features_);

    {
      // Initialize and lock the physics engine
      GetWorld()->Physics()->InitForThread();
      boost::unique_lock<boost::recursive_mutex> lock(*(GetWorld()->Physics()->GetPhysicsUpdateMutex()));

      // Iterate over the map in an attempt to find features in the frustrum
      for (size_t i = 0; i < num_features_; i++) {
        // Point in the sensor frame
        Eigen::Vector3d pt = wTs.inverse() * map_[i].second;

        // If the feature is uninitialized or not in the FOV
        if (map_[i].first == 0 || !camera.IsInFov(pt)) {
          // Get a ray through a random image coordinate
          Eigen::Vector3d ray = camera.Ray((static_cast<double>(rand() % 1000) / 1000 - 0.5)  // NOLINT
                                             * camera.GetParameters().GetDistortedSize()[0],
                                           (static_cast<double>(rand() % 1000) / 1000 - 0.5)  // NOLINT
                                             * camera.GetParameters().GetDistortedSize()[1]);

          // Get the camera coordinate of the ray near and far clips
          Eigen::Vector3d n_c = near_clip_ * ray;
          Eigen::Vector3d f_c = far_clip_ * ray;

          // Get the world coordinate of the ray near and far clips
          Eigen::Vector3d n_w = wTs * n_c;
          Eigen::Vector3d f_w = wTs * f_c;

          // Intersection information
          double dist;
          std::string entity;
          shape_->SetPoints(ignition::math::Vector3d(n_w.x(), n_w.y(), n_w.z()),
                            ignition::math::Vector3d(f_w.x(), f_w.y(), f_w.z()));
          shape_->GetIntersection(dist, entity);
          if (entity.empty()) return false;

          // Get the landmark coordinate
          pt = n_c + dist * (f_c - n_c).normalized();

          // Add the feature to the map for future use
          map_[i].first = ++id_;
          map_[i].second = n_w + dist * (f_w - n_w).normalized();
        }

        // Get the image coordinates of the feature
        Eigen::Vector2d uv = camera.ImageCoordinates(pt);

        // Construct a feature
        ff_msgs::Feature2d feature;
        feature.id = map_[i].first;
        feature.x = uv.x();
        feature.y = uv.y();

        // Add the feature to the message
        msg.feature_array.push_back(feature);
      }
    }
    // Success!
    return true;
  }

 private:
  config_reader::ConfigReader config_;

  rclcpp::Publisher<ff_msgs::CameraRegistration>::SharedPtr pub_reg_;
  rclcpp::Publisher<ff_msgs::Feature2dArray>::SharedPtr pub_feat_;
  rclcpp::Service<ff_msgs::SetBool>::SharedPtr srv_enable_;
  ff_util::FreeFlyerTimer timer_registration_, timer_features_;

  std::shared_ptr<sensors::WideAngleCameraSensor> sensor_;
  gazebo::physics::RayShapePtr shape_;
  bool active_;
  ff_msgs::CameraRegistration msg_reg_;
  ff_msgs::Feature2dArray msg_feat_;
  double rate_;
  double delay_camera_;
  double delay_features_;
  double near_clip_;
  double far_clip_;
  unsigned int num_features_;
  std::vector<std::pair<uint16_t, Eigen::Vector3d>> map_;
  uint16_t id_;
};

GZ_REGISTER_SENSOR_PLUGIN(GazeboSensorPluginOpticalFlow)

}  // namespace gazebo
