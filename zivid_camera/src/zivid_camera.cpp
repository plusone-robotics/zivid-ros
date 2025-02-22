#include "zivid_camera.h"
#include "CaptureGeneralConfigUtils.h"
#include "CaptureFrameConfigUtils.h"

#include <sensor_msgs/point_cloud2_iterator.h>
#include <sensor_msgs/image_encodings.h>
#include <sensor_msgs/distortion_models.h>
#include <dynamic_reconfigure/config_tools.h>

#include <Zivid/HDR.h>
#include <Zivid/Firmware.h>
#include <Zivid/Version.h>

#include <boost/algorithm/string.hpp>
#include <boost/predef.h>

#include <sstream>
#include <thread>
#include <cstdint>

namespace
{
sensor_msgs::PointField createPointField(std::string name, uint32_t offset, uint8_t datatype, uint32_t count)
{
  sensor_msgs::PointField point_field;
  point_field.name = name;
  point_field.offset = offset;
  point_field.datatype = datatype;
  point_field.count = count;
  return point_field;
}

bool big_endian()
{
  return BOOST_ENDIAN_BIG_BYTE;
}

template <class T>
void fillCommonMsgFields(T& msg, const std_msgs::Header& header, const Zivid::PointCloud& pc)
{
  msg.header = header;
  msg.height = static_cast<uint32_t>(pc.height());
  msg.width = static_cast<uint32_t>(pc.width());
  msg.is_bigendian = big_endian();
}

std::string toString(zivid_camera::CameraStatus camera_status)
{
  switch (camera_status)
  {
    case zivid_camera::CameraStatus::Connected:
      return "Connected";
    case zivid_camera::CameraStatus::Disconnected:
      return "Disconnected";
    case zivid_camera::CameraStatus::Idle:
      return "Idle";
  }
  return "N/A";
}

}  // namespace

namespace zivid_camera
{
ZividCamera::ZividCamera(ros::NodeHandle& nh, ros::NodeHandle& priv)
  : nh_(nh)
  , priv_(priv)
  , camera_status_(CameraStatus::Idle)
  , current_capture_general_config_(decltype(current_capture_general_config_)::__getDefault__())
  , use_latched_publisher_for_points_(false)
  , use_latched_publisher_for_color_image_(false)
  , use_latched_publisher_for_depth_image_(false)
  , image_transport_(nh_)
  , header_seq_(0)
{
  ROS_INFO("Zivid ROS driver version %s", ZIVID_ROS_DRIVER_VERSION);

  ROS_INFO("Node's namespace is '%s'", nh_.getNamespace().c_str());
  if (ros::this_node::getNamespace() == "/")
  {
    // Require the user to specify the namespace that this node will run in.
    // See REP-135 http://www.ros.org/reps/rep-0135.html
    throw std::runtime_error("Zivid ROS driver started in the global namespace ('/')! This is unsupported. "
                             "Please specify namespace, e.g. using the ROS_NAMESPACE environment variable.");
  }

  ROS_INFO_STREAM("Built towards Zivid API version " << ZIVID_VERSION);
  ROS_INFO_STREAM("Running with Zivid API version " << Zivid::Version::libraryVersion());
  if (Zivid::Version::libraryVersion() != ZIVID_VERSION)
  {
    throw std::runtime_error("Zivid library mismatch! The running Zivid Core version does not match the "
                             "version this ROS driver was built towards. Hint: Try to clean and re-build your project "
                             "from scratch.");
  }

  std::string serial_number;
  priv_.param<decltype(serial_number)>("serial_number", serial_number, "");

  int num_capture_frames;
  priv_.param<decltype(num_capture_frames)>("num_capture_frames", num_capture_frames, 10);

  priv_.param<decltype(frame_id_)>("frame_id", frame_id_, "zivid_optical_frame");

  std::string file_camera_path;
  priv_.param<decltype(file_camera_path)>("file_camera_path", file_camera_path, "");
  const bool file_camera_mode = !file_camera_path.empty();

  priv_.param<bool>("use_latched_publisher_for_points", use_latched_publisher_for_points_, false);
  priv_.param<bool>("use_latched_publisher_for_color_image", use_latched_publisher_for_color_image_, false);
  priv_.param<bool>("use_latched_publisher_for_depth_image", use_latched_publisher_for_depth_image_, false);

  if (file_camera_mode)
  {
    ROS_INFO("Creating file camera from file '%s'", file_camera_path.c_str());
    camera_ = zivid_.createFileCamera(file_camera_path);
  }
  else
  {
    auto cameras = zivid_.cameras();
    ROS_INFO_STREAM(cameras.size() << " cameras found");
    if (cameras.empty())
    {
      throw std::runtime_error("No cameras found. Ensure that the camera is connected to the USB3 port on your PC.");
    }
    else if (serial_number.empty())
    {
      camera_ = [&]() {
        ROS_INFO("Selecting first available camera");
        for (auto& c : cameras)
        {
          if (c.state().isAvailable())
            return c;
        }
        throw std::runtime_error("No available cameras found. Is the camera in use by another process?");
      }();
    }
    else
    {
      if (serial_number.find(":") == 0)
      {
        serial_number = serial_number.substr(1);
      }
      camera_ = [&]() {
        ROS_INFO("Searching for camera with serial number '%s' ...", serial_number.c_str());
        for (auto& c : cameras)
        {
          if (c.serialNumber() == Zivid::SerialNumber(serial_number))
            return c;
        }
        throw std::runtime_error("No camera found with serial number '" + serial_number + "'");
      }();
    }

    if (!Zivid::Firmware::isUpToDate(camera_))
    {
      ROS_INFO("The camera firmware is not up-to-date, starting update");
      Zivid::Firmware::update(camera_, [](double progress, const std::string& state) {
        ROS_INFO("  [%.0f%%] %s", progress, state.c_str());
      });
      ROS_INFO("Firmware update completed");
    }
  }

  ROS_INFO_STREAM(camera_);
  if (!file_camera_mode)
  {
    ROS_INFO_STREAM("Connecting to camera '" << camera_.serialNumber() << "'");
    camera_.connect();
  }
  ROS_INFO_STREAM("Connected to camera '" << camera_.serialNumber() << "'");
  setCameraStatus(CameraStatus::Connected);

  camera_connection_keepalive_timer_ =
      nh_.createTimer(ros::Duration(10), &ZividCamera::onCameraConnectionKeepAliveTimeout, this);

  const auto camera_settings = camera_.settings();
  setupCaptureGeneralConfigNode(camera_settings);

  ROS_INFO("Setting up %d capture_frame dynamic_reconfigure nodes", num_capture_frames);
  for (int i = 0; i < num_capture_frames; i++)
  {
    setupCaptureFrameConfigNode(i, camera_settings);
  }

  ROS_INFO("Advertising topics");
  points_publisher_ = nh_.advertise<sensor_msgs::PointCloud2>("points", 1, use_latched_publisher_for_points_);
  color_image_publisher_ =
      image_transport_.advertiseCamera("color/image_color", 1, use_latched_publisher_for_color_image_);
  depth_image_publisher_ =
      image_transport_.advertiseCamera("depth/image_raw", 1, use_latched_publisher_for_depth_image_);

  ROS_INFO("Advertising services");
  camera_info_model_name_service_ =
      nh_.advertiseService("camera_info/model_name", &ZividCamera::cameraInfoModelNameServiceHandler, this);
  camera_info_serial_number_service_ =
      nh_.advertiseService("camera_info/serial_number", &ZividCamera::cameraInfoSerialNumberServiceHandler, this);
  is_connected_service_ = nh_.advertiseService("is_connected", &ZividCamera::isConnectedServiceHandler, this);
  capture_service_ = nh_.advertiseService("capture", &ZividCamera::captureServiceHandler, this);

  ROS_INFO("Zivid camera driver is now ready!");
}

void ZividCamera::onCameraConnectionKeepAliveTimeout(const ros::TimerEvent&)
{
  ROS_DEBUG_STREAM(__func__ << ", threadid=" << std::this_thread::get_id());
  try
  {
    reconnectToCameraIfNecessary();
  }
  catch (const std::exception& e)
  {
    ROS_INFO("%s failed with exception '%s'", __func__, e.what());
  }
}

void ZividCamera::reconnectToCameraIfNecessary()
{
  ROS_DEBUG_STREAM(__func__ << ", threadid=" << std::this_thread::get_id());

  const auto state = camera_.state();
  if (state.isConnected().value())
  {
    setCameraStatus(CameraStatus::Connected);
  }
  else
  {
    setCameraStatus(CameraStatus::Disconnected);

    // The camera handle needs to be refreshed to ensure we get the correct
    // "available" status. This is a bug in the API.
    auto cameras = zivid_.cameras();
    for (auto& c : cameras)
    {
      if (camera_.serialNumber() == c.serialNumber())
      {
        camera_ = c;
      }
    }

    if (state.isAvailable().value())
    {
      ROS_INFO_STREAM("The camera '" << camera_.serialNumber()
                                     << "' is not connected but is available. Re-connecting ...");
      camera_.connect();
      ROS_INFO("Successfully reconnected to camera!");
      setCameraStatus(CameraStatus::Connected);
    }
    else
    {
      ROS_INFO_STREAM("The camera '" << camera_.serialNumber() << "' is not connected nor available.");
    }
  }
}

void ZividCamera::setCameraStatus(CameraStatus camera_status)
{
  if (camera_status_ != camera_status)
  {
    std::stringstream ss;
    ss << "Camera status changed to " << toString(camera_status) << " (was " << toString(camera_status_) << ")";
    if (camera_status == CameraStatus::Connected)
    {
      ROS_INFO_STREAM(ss.str());
    }
    else
    {
      ROS_WARN_STREAM(ss.str());
    }
    camera_status_ = camera_status;
  }
}

void ZividCamera::setupCaptureGeneralConfigNode(const Zivid::Settings& defaultSettings)
{
  capture_general_dr_server_ = std::make_unique<dynamic_reconfigure::Server<CaptureGeneralConfig>>(
      capture_general_dr_server_mutex_, ros::NodeHandle(nh_, "capture/general"));

  // Setup min, max, default and current config
  const auto min_config = getCaptureGeneralConfigMinFromZividSettings(defaultSettings);
  capture_general_dr_server_->setConfigMin(min_config);

  const auto max_config = getCaptureGeneralConfigMaxFromZividSettings(defaultSettings);
  capture_general_dr_server_->setConfigMax(max_config);

  const auto default_config = getCaptureGeneralConfigDefaultFromZividSettings(defaultSettings);
  capture_general_dr_server_->setConfigDefault(default_config);
  capture_general_dr_server_->updateConfig(default_config);

  // Setup the cb, this will invoke the cb, ensuring that the locally cached
  // config is updated.
  capture_general_dr_server_->setCallback(boost::bind(&ZividCamera::onCaptureGeneralConfigChanged, this, _1));
}

void ZividCamera::setupCaptureFrameConfigNode(int nodeIdx, const Zivid::Settings& defaultSettings)
{
  auto frame_config = std::make_unique<DRFrameConfig>("capture/frame_" + std::to_string(nodeIdx), nh_);

  // Setup min, max, default and current config
  const auto minConfig = getCaptureFrameConfigMinFromZividSettings(defaultSettings);
  frame_config->dr_server.setConfigMin(minConfig);

  const auto max_config = getCaptureFrameConfigMaxFromZividSettings(defaultSettings);
  frame_config->dr_server.setConfigMax(max_config);

  const auto default_config = getCaptureFrameConfigDefaultFromZividSettings(defaultSettings);
  frame_config->dr_server.setConfigDefault(default_config);
  frame_config->dr_server.updateConfig(default_config);
  frame_config->dr_server.setCallback(
      boost::bind(&ZividCamera::onCaptureFrameConfigChanged, this, _1, std::ref(*frame_config.get())));

  frame_configs_.push_back(std::move(frame_config));
}

void ZividCamera::onCaptureGeneralConfigChanged(CaptureGeneralConfig& config)
{
  ROS_INFO("%s", __func__);
  current_capture_general_config_ = config;
}

void ZividCamera::onCaptureFrameConfigChanged(CaptureFrameConfig& config, DRFrameConfig& frame_config)
{
  ROS_INFO("%s name='%s'", __func__, frame_config.name.c_str());
  frame_config.config = config;
}

bool ZividCamera::cameraInfoModelNameServiceHandler(zivid_camera::CameraInfoModelName::Request&,
                                                    zivid_camera::CameraInfoModelName::Response& res)
{
  res.model_name = camera_.modelName();
  return true;
}

bool ZividCamera::cameraInfoSerialNumberServiceHandler(zivid_camera::CameraInfoSerialNumber::Request&,
                                                       zivid_camera::CameraInfoSerialNumber::Response& res)
{
  res.serial_number = camera_.serialNumber().toString();
  return true;
}

bool ZividCamera::captureServiceHandler(Capture::Request&, Capture::Response&)
{
  ROS_DEBUG_STREAM(__func__ << ", threadid=" << std::this_thread::get_id());

  reconnectToCameraIfNecessary();
  if (camera_status_ != CameraStatus::Connected)
  {
    throw std::runtime_error("Unable to capture since the camera is not connected. Please re-connect the camera and "
                             "try again.");
  }

  std::vector<Zivid::Settings> settings;

  Zivid::Settings base_setting = camera_.settings();
  applyCaptureGeneralConfigToZividSettings(current_capture_general_config_, base_setting);

  for (const auto& frame_config : frame_configs_)
  {
    if (frame_config->config.enabled)
    {
      ROS_DEBUG("Config %s is enabled", frame_config->name.c_str());
      Zivid::Settings s{ base_setting };
      applyCaptureFrameConfigToZividSettings(frame_config->config, s);
      settings.push_back(s);
    }
  }

  if (settings.size() == 0)
  {
    throw std::runtime_error("Capture called with 0 enabled frames!");
  }

  ROS_INFO("Capturing with %zd frames", settings.size());
  std::vector<Zivid::Frame> frames;
  frames.reserve(settings.size());
  for (const auto& s : settings)
  {
    camera_.setSettings(s);
    ROS_DEBUG("Calling capture() with settings: %s", camera_.settings().toString().c_str());
    frames.emplace_back(camera_.capture());
  }

  auto frame = [&]() {
    if (frames.size() > 1)
    {
      ROS_DEBUG("Calling Zivid::HDR::combineFrames with %zd frames", frames.size());
      return Zivid::HDR::combineFrames(begin(frames), end(frames));
    }
    else
    {
      return frames[0];
    }
  }();
  publishFrame(std::move(frame));
  return true;
}

bool ZividCamera::isConnectedServiceHandler(IsConnected::Request&, IsConnected::Response& res)
{
  res.is_connected = camera_status_ == CameraStatus::Connected;
  return true;
}

void ZividCamera::publishFrame(Zivid::Frame&& frame)
{
  const bool publish_points = points_publisher_.getNumSubscribers() > 0 || use_latched_publisher_for_points_;
  const bool publish_color_img =
      color_image_publisher_.getNumSubscribers() > 0 || use_latched_publisher_for_color_image_;
  const bool publish_depth_img =
      depth_image_publisher_.getNumSubscribers() > 0 || use_latched_publisher_for_depth_image_;

  if (publish_points || publish_color_img || publish_depth_img)
  {
    auto point_cloud = frame.getPointCloud();

    std_msgs::Header header;
    header.seq = header_seq_++;
    header.stamp = ros::Time::now();
    header.frame_id = frame_id_;

    if (publish_points)
    {
      ROS_DEBUG("Publishing points");
      points_publisher_.publish(makePointCloud2(header, point_cloud));
    }

    if (publish_color_img || publish_depth_img)
    {
      const auto camera_info = makeCameraInfo(header, point_cloud, camera_.intrinsics());

      if (publish_color_img)
      {
        ROS_DEBUG("Publishing color image");
        color_image_publisher_.publish(makeColorImage(header, point_cloud), camera_info);
      }

      if (publish_depth_img)
      {
        ROS_DEBUG("Publishing depth image");
        depth_image_publisher_.publish(makeDepthImage(header, point_cloud), camera_info);
      }
    }
  }
}

sensor_msgs::PointCloud2ConstPtr ZividCamera::makePointCloud2(const std_msgs::Header& header,
                                                              const Zivid::PointCloud& point_cloud)
{
  auto msg = boost::make_shared<sensor_msgs::PointCloud2>();
  fillCommonMsgFields(*msg, header, point_cloud);
  msg->point_step = sizeof(Zivid::Point);
  msg->row_step = msg->point_step * msg->width;
  msg->is_dense = false;

  msg->fields.reserve(5);
  msg->fields.push_back(createPointField("x", 0, 7, 1));
  msg->fields.push_back(createPointField("y", 4, 7, 1));
  msg->fields.push_back(createPointField("z", 8, 7, 1));
  msg->fields.push_back(createPointField("c", 12, 7, 1));
  msg->fields.push_back(createPointField("rgb", 16, 7, 1));

  msg->data = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(point_cloud.dataPtr()),
                                   reinterpret_cast<const uint8_t*>(point_cloud.dataPtr() + point_cloud.size()));

#pragma omp parallel for
  for (std::size_t i = 0; i < point_cloud.size(); i++)
  {
    uint8_t* point_ptr = &(msg->data[i * sizeof(Zivid::Point)]);
    float* x_ptr = reinterpret_cast<float*>(&(point_ptr[0]));
    float* y_ptr = reinterpret_cast<float*>(&(point_ptr[4]));
    float* z_ptr = reinterpret_cast<float*>(&(point_ptr[8]));

    // Convert from mm to m
    *x_ptr *= 0.001f;
    *y_ptr *= 0.001f;
    *z_ptr *= 0.001f;
  }
  return msg;
}

sensor_msgs::ImageConstPtr ZividCamera::makeColorImage(const std_msgs::Header& header,
                                                       const Zivid::PointCloud& point_cloud)
{
  auto msg = boost::make_shared<sensor_msgs::Image>();
  fillCommonMsgFields(*msg, header, point_cloud);
  msg->encoding = sensor_msgs::image_encodings::RGB8;
  msg->step = static_cast<uint32_t>(3 * point_cloud.width());
  msg->data.resize(msg->step * msg->height);

#pragma omp parallel for
  for (std::size_t i = 0; i < point_cloud.size(); i++)
  {
    const auto point = point_cloud(i);
    msg->data[3 * i] = point.red();
    msg->data[3 * i + 1] = point.green();
    msg->data[3 * i + 2] = point.blue();
  }
  return msg;
}

sensor_msgs::ImageConstPtr ZividCamera::makeDepthImage(const std_msgs::Header& header,
                                                       const Zivid::PointCloud& point_cloud)
{
  auto msg = boost::make_shared<sensor_msgs::Image>();
  fillCommonMsgFields(*msg, header, point_cloud);
  msg->encoding = sensor_msgs::image_encodings::TYPE_32FC1;
  msg->step = static_cast<uint32_t>(4 * point_cloud.width());
  msg->data.resize(msg->step * msg->height);

#pragma omp parallel for
  for (std::size_t i = 0; i < point_cloud.size(); i++)
  {
    float* image_data = reinterpret_cast<float*>(&msg->data[4 * i]);
    // Convert from mm to m
    *image_data = point_cloud(i).z * 0.001f;
  }
  return msg;
}

sensor_msgs::CameraInfoConstPtr ZividCamera::makeCameraInfo(const std_msgs::Header& header,
                                                            const Zivid::PointCloud& point_cloud,
                                                            const Zivid::CameraIntrinsics& intrinsics)
{
  auto msg = boost::make_shared<sensor_msgs::CameraInfo>();
  msg->header = header;
  msg->width = static_cast<uint32_t>(point_cloud.width());
  msg->height = static_cast<uint32_t>(point_cloud.height());
  msg->distortion_model = sensor_msgs::distortion_models::PLUMB_BOB;

  // k1, k2, t1, t2, k3
  const auto distortion = intrinsics.distortion();
  msg->D.resize(5);
  msg->D[0] = distortion.k1().value();
  msg->D[1] = distortion.k2().value();
  msg->D[2] = distortion.p1().value();
  msg->D[3] = distortion.p2().value();
  msg->D[4] = distortion.k3().value();

  // Intrinsic camera matrix for the raw (distorted) images.
  //     [fx  0 cx]
  // K = [ 0 fy cy]
  //     [ 0  0  1]
  const auto camera_matrix = intrinsics.cameraMatrix();
  msg->K[0] = camera_matrix.fx().value();
  msg->K[2] = camera_matrix.cx().value();
  msg->K[4] = camera_matrix.fy().value();
  msg->K[5] = camera_matrix.cy().value();
  msg->K[8] = 1;

  // R (identity)
  msg->R[0] = 1;
  msg->R[4] = 1;
  msg->R[8] = 1;

  // Projection/camera matrix
  //     [fx'  0  cx' Tx]
  // P = [ 0  fy' cy' Ty]
  //     [ 0   0   1   0]
  msg->P[0] = camera_matrix.fx().value();
  msg->P[2] = camera_matrix.cx().value();
  msg->P[5] = camera_matrix.fy().value();
  msg->P[6] = camera_matrix.cy().value();
  msg->P[10] = 1;

  return msg;
}

}  // namespace zivid_camera
