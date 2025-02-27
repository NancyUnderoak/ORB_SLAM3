/**
* This file is part of ORB-SLAM3
*
* Copyright (C) 2017-2021 Carlos Campos, Richard Elvira, Juan J. Gómez Rodríguez, José M.M. Montiel and Juan D. Tardós, University of Zaragoza.
* Copyright (C) 2014-2016 Raúl Mur-Artal, José M.M. Montiel and Juan D. Tardós, University of Zaragoza.
*
* ORB-SLAM3 is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
* License as published by the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* ORB-SLAM3 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
* the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along with ORB-SLAM3.
* If not, see <http://www.gnu.org/licenses/>.
*/

#include<iostream>
#include<algorithm>
#include<fstream>
#include<chrono>
#include<vector>
#include<queue>
#include<thread>
#include<mutex>

#include<ros/ros.h>
#include<cv_bridge/cv_bridge.h>
#include<sensor_msgs/Imu.h>

#include<opencv2/core/core.hpp>
#include "opencv2/imgcodecs/legacy/constants_c.h"

#include <tf2_ros/transform_broadcaster.h>
#include <geometry_msgs/TransformStamped.h>

#include"../../../include/System.h"
#include"../include/ImuTypes.h"

using namespace std;

class ImuGrabber
{
public:
    ImuGrabber(){};
    void GrabImu(const sensor_msgs::ImuConstPtr &imu_msg);

    queue<sensor_msgs::ImuConstPtr> imuBuf;
    std::mutex mBufMutex;
};

class ImageGrabber
{
public:
    tf2_ros::TransformBroadcaster broadcaster;
    geometry_msgs::TransformStamped transformStamped;
    ros::Rate rate = ros::Rate(10.0);
    
    ImageGrabber(ORB_SLAM3::System* pSLAM, ImuGrabber *pImuGb, const bool bClahe, const bool isLocalisation):
    mpSLAM(pSLAM), mpImuGb(pImuGb), mbClahe(bClahe){
      if(isLocalisation){
        transformStamped.header.frame_id = "parent_frame";  // parent_frame
        transformStamped.child_frame_id  = "child_frame";   // child_frame
      }
      else{
        transformStamped.header.frame_id = "orb_static_frame";  // parent_frame
        transformStamped.child_frame_id  = "orb_dynamic_frame";   // child_frame
      }
    }

    void GrabImage(const sensor_msgs::ImageConstPtr& msg);
    cv::Mat GetImage(const sensor_msgs::ImageConstPtr &img_msg);
    void SyncWithImu();

    queue<sensor_msgs::ImageConstPtr> img0Buf;
    std::mutex mBufMutex;
   
    ORB_SLAM3::System* mpSLAM;
    ImuGrabber *mpImuGb;

    const bool mbClahe;
    cv::Ptr<cv::CLAHE> mClahe = cv::createCLAHE(3.0, cv::Size(8, 8));
};



int main(int argc, char **argv)
{
  ros::init(argc, argv, "Mono_Inertial");
  ros::NodeHandle n("~");
  ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Info);

  if(argc != 5)
  {
    cerr << endl << "Usage: rosrun ORB_SLAM3 Mono_Inertial path_to_vocabulary path_to_settings show_gui[true/false] islocalisation[true/false] //[do_equalize]" << endl;
    ros::shutdown();
    return 1;
  }

  std::string suseGui(argv[3]);
  std::string sisLocalization(argv[4]);


  bool useGui = true;
  bool isLocalization = false;
  bool bEqual = false;
  
  int queue_imu_size = 1;

  if(suseGui == "false"){
      useGui = false;
      std::cout<<"Gui disabled"<<std::endl;
  }
  else
      std::cout<<"Gui enabled"<<std::endl;

  if(sisLocalization == "true"){
      isLocalization = true;
      queue_imu_size = 100;
      std::cout<<"Localisation mode"<<std::endl;
  }
  else
      std::cout<<"Mapping mode"<<std::endl;

  string filename;

  // Create SLAM system. It initializes all system threads and gets ready to process frames.
  ORB_SLAM3::System SLAM(argv[1],argv[2],ORB_SLAM3::System::IMU_MONOCULAR,useGui, 0, filename, isLocalization);

  ImuGrabber imugb;
  ImageGrabber igb(&SLAM,&imugb,bEqual, isLocalization); // TODO
  
  // Maximum delay, 5 seconds
  ros::Subscriber sub_imu = n.subscribe("/ardrone/imu", queue_imu_size, &ImuGrabber::GrabImu, &imugb); 
  ros::Subscriber sub_img0 = n.subscribe("/ardrone/front/image_raw", 1, &ImageGrabber::GrabImage,&igb);

  std::thread sync_thread(&ImageGrabber::SyncWithImu,&igb);

  ros::spin();

  return 0;
}

void ImageGrabber::GrabImage(const sensor_msgs::ImageConstPtr &img_msg)
{
  mBufMutex.lock();
  if (!img0Buf.empty())
    img0Buf.pop();
  img0Buf.push(img_msg);
  mBufMutex.unlock();
}

cv::Mat ImageGrabber::GetImage(const sensor_msgs::ImageConstPtr &img_msg)
{
  // Copy the ros image message to cv::Mat.
  cv_bridge::CvImageConstPtr cv_ptr;
  try
  {
    cv_ptr = cv_bridge::toCvShare(img_msg, sensor_msgs::image_encodings::MONO8);
  }
  catch (cv_bridge::Exception& e)
  {
    ROS_ERROR("cv_bridge exception: %s", e.what());
  }
  
  if(cv_ptr->image.type()==0)
  {
    return cv_ptr->image.clone();
  }
  else
  {
    std::cout << "Error type" << std::endl;
    return cv_ptr->image.clone();
  }
}

void ImageGrabber::SyncWithImu()
{
  while(1)
  {
    cv::Mat im;
    double tIm = 0;
    if (!img0Buf.empty()&&!mpImuGb->imuBuf.empty())
    {
      tIm = img0Buf.front()->header.stamp.toSec();
      if(tIm>mpImuGb->imuBuf.back()->header.stamp.toSec())
          continue;
      {
      this->mBufMutex.lock();
      im = GetImage(img0Buf.front());
      img0Buf.pop();
      this->mBufMutex.unlock();
      }

      vector<ORB_SLAM3::IMU::Point> vImuMeas;
      mpImuGb->mBufMutex.lock();
      if(!mpImuGb->imuBuf.empty())
      {
        // Load imu measurements from buffer
        vImuMeas.clear();
        while(!mpImuGb->imuBuf.empty() && mpImuGb->imuBuf.front()->header.stamp.toSec()<=tIm)
        {
          double t = mpImuGb->imuBuf.front()->header.stamp.toSec();
          cv::Point3f acc(mpImuGb->imuBuf.front()->linear_acceleration.x, mpImuGb->imuBuf.front()->linear_acceleration.y, mpImuGb->imuBuf.front()->linear_acceleration.z);
          cv::Point3f gyr(mpImuGb->imuBuf.front()->angular_velocity.x, mpImuGb->imuBuf.front()->angular_velocity.y, mpImuGb->imuBuf.front()->angular_velocity.z);
          vImuMeas.push_back(ORB_SLAM3::IMU::Point(acc,gyr,t));
          mpImuGb->imuBuf.pop();
        }
      }
      mpImuGb->mBufMutex.unlock();
      if(mbClahe)
        mClahe->apply(im,im);

      Sophus::SE3f last_tf = mpSLAM->TrackMonocular(im,tIm,vImuMeas);
      last_tf = last_tf.inverse();

      Eigen::Quaterniond quaternion(last_tf.rotationMatrix().cast<double>());
      Eigen::Vector3f translation = last_tf.translation();
      // last_tf = convertion_tf*last_tf;

      // //rotate by x
      // Eigen::Quaterniond rotation(
      //   std::sqrt(0.5), std::sqrt(0.5), 0.0, 0.0
      // );

      transformStamped.header.stamp = ros::Time::now();

      transformStamped.transform.translation.x = translation.x();
      transformStamped.transform.translation.y = translation.y();
      transformStamped.transform.translation.z = translation.z();

      //std::cout << translation.x() << std::endl;

      transformStamped.transform.rotation.x = quaternion.x();
      transformStamped.transform.rotation.y = quaternion.y();
      transformStamped.transform.rotation.z = quaternion.z();
      transformStamped.transform.rotation.w = quaternion.w();

      broadcaster.sendTransform(transformStamped);

      rate.sleep();
    }

    std::chrono::milliseconds tSleep(1);
    std::this_thread::sleep_for(tSleep);
  }
}

void ImuGrabber::GrabImu(const sensor_msgs::ImuConstPtr &imu_msg)
{
  mBufMutex.lock();
  imuBuf.push(imu_msg);
  mBufMutex.unlock();
  return;
}


