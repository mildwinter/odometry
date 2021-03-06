// Test Camera Tracking over a sequence of pairs of consecutive images
#include <iostream>
#include <vector>
#include <Eigen/Core>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/highgui.hpp>
#include <fstream>
#include "include/camera.h"
#include "include/data_types.h"
#include "include/image_processing_global.h"
#include "include/image_pyramid.h"
#include "include/lm_optimizer.h"
#include <se3.hpp>
#include <typeinfo>

#ifndef N_FRAMES
#define N_FRAMES 32 // 200
#endif

/******************************* CHOOSE DATASET ***********************************/
//const std::string kDataPath = "../dataset/rgbd_dataset_freiburg3_sitting_static";
const std::string kDataPath = "../dataset/rgbd_dataset_freiburg3_teddy";
//const std::string kDataPath = "../dataset/rgbd_dataset_freiburg3_sitting_xyz";
//const std::string kDataPath = "../dataset/rgbd_dataset_freiburg3_long_office_household";


void load_data(std::string filename, std::vector<cv::Mat> &gray, std::vector<cv::Mat> &depth, Eigen::MatrixXf &poses, int n_frames);

int main() {

  /******************************* CREATE CAMERA INSTANCE ***********************************/
  // use Freiburg 3 sequence (already undistorted, rgb and depth are already pre-registered)
  float fx = 535.4, fy = 539.2, f_theta = 0, cx = 320.1, cy = 247.6;
  std::shared_ptr<odometry::CameraPyramid> camera_ptr = std::make_shared<odometry::CameraPyramid>(4, fx, fy, f_theta, cx, cy);


  /******************************* LOAD DATASET ***********************************/
  Eigen::MatrixXf poses(7, N_FRAMES); // qw, qx, qy, qz, tx, ty, tz; absolute pose of cameras w.r.t. world origin
  std::vector<cv::Mat> gray(N_FRAMES); // float intensity
  std::vector<cv::Mat> depth(N_FRAMES); // float depth
  load_data(kDataPath + "/associated.txt", gray, depth, poses, N_FRAMES);
  std::cout << "Load data done: " << N_FRAMES << " frames" << std::endl;


  /******************************* CREATE IMAGE/DEPTH PYRAMIDS ***********************************/
  // all input rgb must already be converted to grayscale(float), depth must also be float(in meters, invalid = 0)
  // avoid copying using unique_ptr
  std::vector<std::unique_ptr<odometry::ImagePyramid>> img_pyramids;
  std::vector<std::unique_ptr<odometry::DepthPyramid>> dep_pyramids;
  for (int idx = 0; idx < N_FRAMES; idx++){
    img_pyramids.emplace_back(std::make_unique<odometry::ImagePyramid>(4, gray[idx], false));
    dep_pyramids.emplace_back(std::make_unique<odometry::DepthPyramid>(4, depth[idx], false));
  }


  /******************************* CREATE OPTIMIZER INSTANCE ***********************************/
  std::vector<int> max_iters = {10, 20, 30, 30};
  odometry::Affine4f init_relative_affine;
  init_relative_affine.block<3,3>(0,0) = Eigen::Matrix<float, 3, 3>::Identity();
  init_relative_affine.block<1,4>(3,0) << 0.0f, 0.0f, 0.0f, 1.0f;
  init_relative_affine.block<3,1>(0,3) << 0.0f, 0.0f, 0.0f;
  // robust estimator: 0-no, 1-huber, 2-t_dist; t-dist estimator is the better in general
  int robust_estimator = 2;
  float huber_delta = 28.0f;
  odometry::LevenbergMarquardtOptimizer optimizer(0.01f, 0.995f, max_iters, init_relative_affine, camera_ptr, robust_estimator, huber_delta);
  std::cout << "Created optimizer instance." << std::endl;


  /******************************* ESTIMATE & EVALUATE POSES ***********************************/
  // optimize relative camera pose of pairs of frames
  clock_t begin, end;
  odometry::Affine4f rela_pose;
  odometry::Affine4f pred_pose;
  odometry::Affine4f gt_pose;
  float trans_err = 0.0f;
  std::vector<float> acc_errs(N_FRAMES);
  Eigen::AngleAxisf rotation_mat0(Eigen::Quaternionf(poses(0,0), poses(1,0), poses(2,0), poses(3,0)));
  odometry::Affine4f pose0;
  pose0.block<3,3>(0,0) = rotation_mat0.toRotationMatrix();
  pose0.block<4,1>(0,3) << poses(4,0), poses(5,0), poses(6,0), 1.0f;
  pose0.block<1,3>(3,0) << 0.0f, 0.0f, 0.0f;
  pred_pose = pose0; // set the 0th pose as the starting point
  acc_errs[0] = 0.0f;
  for (int f_id = 1; f_id < N_FRAMES; f_id++){
    std::cout << "Optimize frame " << f_id << " ..." << std::endl;
    // compute current relative pose
    begin = clock();
    rela_pose = optimizer.Solve(*img_pyramids[f_id-1], *dep_pyramids[f_id-1], *img_pyramids[f_id]);
    end = clock();
    std::cout << "run time: " << double(end - begin) / CLOCKS_PER_SEC * 1000.0f << " ms." << std::endl;
    // compute current absolute pose
    pred_pose = pred_pose.eval() * rela_pose.inverse();
    // get current absolute gt pose
    Eigen::AngleAxisf rotation_mat_gt(Eigen::Quaternionf(poses(0,f_id), poses(1,f_id), poses(2,f_id), poses(3,f_id)));
    gt_pose.block<3,3>(0,0) = rotation_mat_gt.toRotationMatrix();
    gt_pose.block<4,1>(0,3) << poses(4,f_id), poses(5,f_id), poses(6,f_id), 1.0f;
    gt_pose.block<1,3>(3,0) << 0.0f, 0.0f, 0.0f;
    // compute translation error
    trans_err = (pred_pose.block<3,1>(0,3) - gt_pose.block<3,1>(0,3)).norm();
    acc_errs[f_id] = trans_err;
    // reset optimizer
    optimizer.Reset(init_relative_affine, 0.01f);
  }

  /******************************* PRINT TRANSLATION ERRORS ***********************************/
  for (int i = 0; i < N_FRAMES; i++){
    std::cout << "accumulated errs(translation): " << acc_errs[i] << std::endl;
  }
  float avg_err = std::accumulate(acc_errs.begin(), acc_errs.end(), 0.0f) / float(N_FRAMES-1);
  std::cout << "average errs(translation) over " << N_FRAMES << " frames: " << avg_err << std::endl;
  return 0;
}

void load_data(std::string filename, std::vector<cv::Mat> &gray, std::vector<cv::Mat> &depth, Eigen::MatrixXf &poses, int n_frames) {
  std::string line;
  std::ifstream file(filename);
  int counter = 0;
  if (file.is_open()) {
    while (std::getline(file, line) && counter < n_frames) {
      std::vector<std::string> items;
      std::string item;
      std::stringstream ss(line);
      while (std::getline(ss, item, ' '))
        items.push_back(item);

      // -> load gray
      std::string filename_rgb = std::string(kDataPath + "/") + items[9];
      cv::Mat gray_8u = cv::imread(filename_rgb, cv::IMREAD_GRAYSCALE);
      if (gray_8u.empty()){
        std::cout << "read img failed for: " << counter << std::endl;
        std::exit(-1);
      }
      gray_8u.convertTo(gray[counter], PixelType);
      // <-

      // -> load depth
      std::string filename_depth = std::string(kDataPath + "/") + items[11];
      cv::Mat depth_img = cv::imread(filename_depth, cv::IMREAD_UNCHANGED);
      if (depth_img.empty()){
        std::cout << "read depth img failed for: " << counter << std::endl;
        std::exit(-1);
      }
      depth_img.convertTo(depth[counter], PixelType, 1.0f/5000.0f);

      // -> pose
      Eigen::Vector3f t(std::stof(items[1]), std::stof(items[2]), std::stof(items[3])); // <- translation T
      //Eigen::Quaternionf q(std::stof(items[7]), std::stof(items[4]), std::stof(items[5]), std::stof(items[6])); // <- rotation in Eigen: w,x,y,z
      //Eigen::Vector3f a = Eigen::AngleAxisf(q).angle()*Eigen::AngleAxisf(q).axis(); // <-- convert to axis angle
      poses.col(counter) << std::stof(items[7]), std::stof(items[4]), std::stof(items[5]), std::stof(items[6]), t(0), t(1), t(2);
      counter++;
    }
    file.close();
  }
  assert(counter == n_frames);
};