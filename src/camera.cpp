#include "camera.h"

cv::Mat eulerAnglesToRotationMatrix(cv::Vec3d &theta)
{
  // Calculate rotation about x axis
  cv::Mat R_x = (cv::Mat_<double>(3, 3) <<
                1, 0,             0,
                0, cos(theta[0]), -sin(theta[0]),
                0, sin(theta[0]), cos(theta[0]));

  // Calculate rotation about y axis
  cv::Mat R_y = (cv::Mat_<double>(3, 3) <<
                cos(theta[1]),  0, sin(theta[1]),
                0,              1, 0,
                -sin(theta[1]), 0, cos(theta[1]));

  // Calculate rotation about z axis
  cv::Mat R_z = (cv::Mat_<double>(3, 3) <<
                cos(theta[2]), -sin(theta[2]), 0,
                sin(theta[2]), cos(theta[2]),  0,
                0,             0,              1);

  // Combined rotation matrix
  cv::Mat R = R_z * R_y * R_x;

  return R;
}

Camera::Camera(std::string name)
{
  name_ = name;
  const std::string fileName = "/home/mintnguyen/Documents/multi-cameras-calibration/aruco-board-markers.yaml";
  Read_ArUco_YAML(fileName, board_config_.dictionary, board_config_.ids, board_config_.objPoints);

  runThreads();
}

Camera::~Camera()
{
  running_ = false;
  for (auto &t : threads_)
    t.join(); // Join threads
}

void Camera::runThreads(void)
{
  running_ = true;                                                                             // Indicate the threads should be running
  threads_.push_back(std::thread(&CameraInterface::extrinsicCalibration, this)); // Create the thread and push it to our vector of threads
}

// Read ArUco board configuration in a yaml file
bool Camera::Read_ArUco_YAML(const std::string &fileName, cv::Ptr<cv::aruco::Dictionary> &dictionary,
                                           std::vector<int> &ids, std::vector<std::vector<cv::Point3f>> &objPoints)
{
  // Default dictionary (not using any others)
  dictionary = cv::aruco::getPredefinedDictionary(cv::aruco::DICT_4X4_100);

  YAML::Node config = YAML::LoadFile(fileName);

  std::vector<std::vector<cv::Point3f>> markerConers;
  if (config["objPoints"])
  {
    std::vector<std::vector<double>> objPointss = config["objPoints"].as<std::vector<std::vector<double>>>();
    markerConers.resize(objPointss.size());
    for (unsigned int i = 0; i < objPointss.size(); i++)
    {
      if (!objPointss.empty())
        for (unsigned int j = 0; j < objPointss.at(i).size(); j += 3)
        {
          cv::Point3f point;
          point.x = objPointss.at(i).at(j);
          point.y = objPointss.at(i).at(j + 1);
          point.z = objPointss.at(i).at(j + 2);
          markerConers.at(i).push_back(point);
        }
    }
    objPoints = markerConers;

    for (unsigned i = 28; i < objPoints.size(); i++)
    {
      std::iter_swap(objPoints.at(i).begin(), objPoints.at(i).begin() + 1);
      std::iter_swap(objPoints.at(i).begin() + 2, objPoints.at(i).begin() + 3);
    }
  }

  if (config["ids"])
    ids = config["ids"].as<std::vector<int>>();

  return true;
}

// Set camera matrix (instrinsic) for the camera
void Camera::setCameraMatrix(cv::Mat camera_matrix)
{
  std::unique_lock<std::mutex> lck(camera1_.i_mtx);
  camera1_.instrinsic = camera_matrix;
}

// Ser image
void Camera::setCameraImage(cv::Mat input_image)
{
  std::unique_lock<std::mutex> lck(camera1_.img_mtx);
  camera1_.image = input_image;
}

cv::Mat Camera::getTransformationMatrix(){
  std::unique_lock<std::mutex> lck(tf_mtx_);
  cv::Mat tf = tf_;
  lck.unlock();

  return tf;
}

void Camera::extrinsicCalibration()
{
  while (running_)
  {
    std::unique_lock<std::mutex> lck1(camera1_.img_mtx);
    cv::Mat input_image = camera1_.image;
    lck1.unlock();

    std::unique_lock<std::mutex> lck2(camera1_.i_mtx);
    cv::Mat instrinsic = camera1_.instrinsic;
    lck2.unlock();

    if (!input_image.empty() && !instrinsic.empty())
    {
      cv::Mat output_image = input_image.clone();

      cv::Mat distCoeffs;
      // Marker detection
      std::vector<int> markerIds; // Create a vector contains marker id
      std::vector<std::vector<cv::Point2f>> markerCorners, rejectedCandidates;
      cv::Ptr<cv::aruco::DetectorParameters> parameters = cv::aruco::DetectorParameters::create();
      cv::Ptr<cv::aruco::Board> board = cv::aruco::Board::create(board_config_.objPoints, board_config_.dictionary, board_config_.ids);
      cv::aruco::detectMarkers(input_image, board_config_.dictionary, markerCorners, markerIds, parameters, rejectedCandidates);

      if (!markerIds.empty())
        cv::aruco::drawDetectedMarkers(output_image, markerCorners, markerIds);

      std::vector<cv::Point3f> objPoints;
      std::vector<cv::Point2f> imgPoints;
      cv::aruco::getBoardObjectAndImagePoints(board, markerCorners, markerIds, objPoints, imgPoints);
      cv::Vec3d rvec, tvec;
      if (!objPoints.empty() && !imgPoints.empty())
      {
        cv::solvePnP(objPoints, imgPoints, instrinsic, distCoeffs, rvec, tvec, false, cv::SOLVEPNP_ITERATIVE);
        cv::aruco::drawAxis(output_image, instrinsic, distCoeffs, rvec, tvec, 0.1);
      }

      cv::Mat rotation_matrix = eulerAnglesToRotationMatrix(rvec);
      // std::cout << rotation_matrix << std::endl;
      cv::Mat extrinsic = cv::Mat::zeros(4, 4, CV_32F); // Homogeneous, combine rotation and translation

      std::unique_lock<std::mutex> lck3(tf_mtx_);
      tf_ = rotation_matrix;
      lck3.unlock();


      // cv::imshow(name_, output_image);
      // cv::waitKey(30);
    }

    if (!running_)
      break;
  }
}