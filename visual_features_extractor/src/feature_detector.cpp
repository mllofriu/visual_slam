/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2014, Kei Okada.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Kei Okada nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/bind.hpp>
#include <boost/thread/thread.hpp>

#include <ros/ros.h>

#include <image_transport/image_transport.h>
#include <cv_bridge/cv_bridge.h>
#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/CameraInfo.h>

#include <opencv2/imgproc/imgproc.hpp>
#include "opencv2/features2d/features2d.hpp"

#include "visual_features_extractor/FeatureDetectorConfig.h"
#include "visual_features_extractor/ORBExtractor.h"
#include "visual_slam_msgs/Frame.h"
#include "visual_slam_msgs/KeyPoint.h"
#include "visual_slam_msgs/TrackingState.h"

boost::asio::io_service ioService;
boost::thread_group threadpool;
boost::asio::io_service::work work(ioService);
int max_threads = 1;
int running_threads = 0;
boost::mutex mtx_exc_;
boost::mutex lock_mtx_;
boost::condition_variable threads_available_cond;
ros::Publisher img_pub_;
ros::Publisher msg_pub_;
ros::Subscriber state_sub_;
//cv::Ptr<cv::FeatureDetector> ORB_detector_;
//cv::ORB * ORB_detector_;

int current_state;
int num_features_, num_features_param_;
int levels_;
float scale_factor_, ini_thrs_, min_thrs_;
std::string orb_extractor_;
bool undistort_points_;

bool publish_image_;
message_filters::Subscriber<sensor_msgs::Image> * image_filter_sub;
message_filters::Subscriber<sensor_msgs::CameraInfo> * camera_info_filter_sub;
message_filters::TimeSynchronizer<sensor_msgs::Image, sensor_msgs::CameraInfo> * msg_sync;

void undistort_keypoints(std::vector<cv::KeyPoint> &keypoints,
                         const sensor_msgs::CameraInfoConstPtr& cam_info,
                         std::vector<cv::KeyPoint> &undistorted_keypoints) {

    // Fill matrix with points
    cv::Mat mat(keypoints.size(), 2, CV_32F);
    for (int i = 0; i < keypoints.size(); i++) {
        mat.at<float>(i, 0) = keypoints[i].pt.x;
        mat.at<float>(i, 1) = keypoints[i].pt.y;
    }

    cv::Mat K = cv::Mat::eye(3, 3, CV_32F);
    K.at<float>(0, 0) = cam_info->K.at(0);
    K.at<float>(1, 1) = cam_info->K.at(4);
    K.at<float>(0, 2) = cam_info->K.at(2);
    K.at<float>(1, 2) = cam_info->K.at(5);
    K.at<float>(2, 2) = 1;

    cv::Mat DistCoef(4, 1, CV_32F);
    // TODO: check that this is ok
    DistCoef.at<float>(0) = cam_info->D.at(0);
    DistCoef.at<float>(1) = cam_info->D.at(1);
    DistCoef.at<float>(2) = cam_info->D.at(2);
    DistCoef.at<float>(3) = cam_info->D.at(3);

    // Undistort points
    mat = mat.reshape(2);
    cv::undistortPoints(mat, mat, K, DistCoef, cv::Mat(), K);
    mat = mat.reshape(1);

    // Fill undistorted keypoint vector
    undistorted_keypoints.resize(keypoints.size());
    for (int i = 0; i < keypoints.size(); i++) {
        cv::KeyPoint kp = keypoints[i];
        kp.pt.x = mat.at<float>(i, 0);
        kp.pt.y = mat.at<float>(i, 1);
        undistorted_keypoints[i] = kp;
    }
}

void proc_img(const sensor_msgs::ImageConstPtr& img,
              const sensor_msgs::CameraInfoConstPtr& cam_info) {
    //ROS_INFO("Started callback");


    // Convert the image into something opencv can handle.
    cv::Mat frame = cv_bridge::toCvShare(img, img->encoding)->image;

    // Convert to gray
    cv::Mat src_gray;
    if (frame.channels() > 1) {
        cv::cvtColor(frame, src_gray, cv::COLOR_RGB2GRAY);
    } else {
        src_gray = frame;
        //cv::cvtColor(src_gray, frame, cv::COLOR_GRAY2BGR);
    }

    // Output structures
    std::vector<cv::KeyPoint> keypoints;
    cv::Mat descriptors;

    // Apply detector
    ORB_SLAM2::ORBextractor ORB_detector_(num_features_,scale_factor_, levels_, ini_thrs_, min_thrs_);
    (ORB_detector_)(src_gray, cv::noArray(), keypoints, descriptors);


    std::vector<cv::KeyPoint> undistorted_keypoints;
    if (undistort_points_ && keypoints.size() > 0){
        undistort_keypoints(keypoints, cam_info, undistorted_keypoints);
    } else {
        undistorted_keypoints = keypoints;
    }

    if (publish_image_) {
        // Draw corners detected
        int r = 4;
        for (size_t i = 0; i < keypoints.size(); i++) {
            cv::circle(frame, keypoints[i].pt, r, cv::Scalar(255, 0, 0));
        }
        // Publish the image.
//        sensor_msgs::Image::Ptr out_img = cv_bridge::CvImage(img->header,
//                                                             "rgb8", frame).toImageMsg();
        img_pub_.publish(cv_bridge::toCvShare(img, img->encoding)->toImageMsg());
    }

    // Create msgs
    visual_slam_msgs::Frame f;
    for (size_t i = 0; i < undistorted_keypoints.size(); i++) {
        visual_slam_msgs::KeyPoint kp;
        kp.x = undistorted_keypoints[i].pt.x;
        kp.y = undistorted_keypoints[i].pt.y;
        kp.size = undistorted_keypoints[i].size;
        kp.angle = undistorted_keypoints[i].angle;
        kp.octave = undistorted_keypoints[i].octave;

        // Pointer to the i-th row
        std::copy(descriptors.ptr<uchar>(i), descriptors.ptr<uchar>(i) + 32,
                  kp.descriptor.begin());

        f.keypoints.push_back(kp);
    }
    f.header.stamp = img->header.stamp;
    f.height = img->height;
    f.width = img->width;

//    ROS_INFO("Frame processed");
    boost::unique_lock<boost::mutex> lock(lock_mtx_);
    msg_pub_.publish(f);
    running_threads--;
    threads_available_cond.notify_all();

    //ROS_INFO("Finished callback");
}

void img_and_info_callback(const sensor_msgs::ImageConstPtr& img,  const sensor_msgs::CameraInfoConstPtr cam_info){
    boost::unique_lock<boost::mutex> lock(lock_mtx_);
    while (running_threads >= max_threads)
        threads_available_cond.wait(lock);

    running_threads++;
    //ROS_INFO("Running threads: %d", running_threads);

    //new std::thread(proc_img, img, cam_info);
    ioService.post(boost::bind(proc_img, img, cam_info));
}

void img_callback(const sensor_msgs::ImageConstPtr& img) {
    const sensor_msgs::CameraInfoConstPtr cam_info;

    boost::unique_lock<boost::mutex> lock(lock_mtx_);
    while (running_threads >= max_threads)
        threads_available_cond.wait(lock);

    running_threads++;
    //ROS_INFO("Running threads: %d", running_threads);

    //new std::thread(proc_img, img, cam_info);
    ioService.post(boost::bind(proc_img, img, cam_info));
}


void tracker_state_callback(const visual_slam_msgs::TrackingState &msg) {
    if (current_state != msg.state) {
        if (msg.state == visual_slam_msgs::TrackingState::NOT_INITIALIZED) {
            //      ORB_detector_ = new cv::ORB(num_features_);
            num_features_ =  num_features_param_;
            ROS_INFO("Using %d features to initialize", num_features_);
        } else {
            //      ORB_detector_ = new cv::ORB(num_features_/2);
            num_features_ = num_features_param_/2;
            ROS_INFO("Using %d features after initialization",
                     num_features_ / 2);
        }
        current_state = msg.state;
    }
}

int main(int argc, char **argv) {
    ros::init(argc, argv, "feature_detector");
    if (ros::names::remap("~image_in") == "image_in"
            || ros::names::remap("~camera_info") == "camera_info") {
        ROS_WARN(
                    "Topics 'image_in' and 'camera_info' have not been remapped! Typical command-line usage:\n"
                    "\t$ rosrun image_rotate image_rotate image:=<image topic> [transport] camera_info:=<camera_info topic>");
    }

    ros::NodeHandle nh("~");

    nh.param("publish_image_", publish_image_, false);
    if (publish_image_) {
        img_pub_ = nh.advertise<sensor_msgs::Image>("image_out", 1);
    }


    nh.param("num_features", num_features_param_, 4000);
    nh.param("levels", levels_, 8);
    nh.param("scale_factor", scale_factor_, 1.2f);
    nh.param("ini_thrs", ini_thrs_, 20.0f);
    nh.param("min_thrs", min_thrs_, 7.0f);
    num_features_ = num_features_param_;

    ROS_INFO("Using a feature extractor from %s with parameters:", orb_extractor_.c_str());
    ROS_INFO("Num features: %d", num_features_);
    ROS_INFO("Scale factor: %f", scale_factor_);
    ROS_INFO("Levels: %d", levels_);
    ROS_INFO("Initial threshold: %f", ini_thrs_);
    ROS_INFO("Min threshold: %f", min_thrs_);



    msg_pub_ = nh.advertise<visual_slam_msgs::Frame>("features", 10);

    bool subscribe_state_;
    nh.param("subscribe_to_state", subscribe_state_, false);
    state_sub_ = nh.subscribe("tracking_state", 1, tracker_state_callback);

    //	ORB_detector_ = new cv::ORB(num_features_,scale_factor_,nlevels_,edge_threshold_);
    //ORB_detector_ = new cv::ORB(num_features_);

    nh.param("undistort_points", undistort_points_, false);
    if(undistort_points_)
        ROS_INFO("Undistorting points");
    current_state = visual_slam_msgs::TrackingState::SYSTEM_NOT_READY;

    ros::NodeHandle("~").param("max_threads", max_threads, 4);
    for (int i = 0; i < max_threads; i ++){
        threadpool.create_thread(
                    boost::bind(&boost::asio::io_service::run, &ioService)
                    );
    }


    ros::Subscriber sub;
    if (undistort_points_){
        image_filter_sub = new message_filters::Subscriber<sensor_msgs::Image>(nh,
                                                                               "image_in", 10);
        camera_info_filter_sub = new message_filters::Subscriber<
                sensor_msgs::CameraInfo>(nh, "camera_info", 10);
        msg_sync = new message_filters::TimeSynchronizer<sensor_msgs::Image,
                sensor_msgs::CameraInfo>(*image_filter_sub, *camera_info_filter_sub,
                                         10);
        msg_sync->registerCallback(boost::bind(&img_and_info_callback, _1, _2));
    } else {
        sub = nh.subscribe("image_in", 10, img_callback);
    }

    ros::spin();

    return 0;
}




