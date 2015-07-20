/**
 * Copyright 2014 University of Bremen, Institute for Artificial Intelligence
 * Author: Thiemo Wiedemeyer <wiedemeyer@cs.uni-bremen.de>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <stdio.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>
#include <mutex>
#include <thread>
#include <chrono>

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/io/pcd_io.h>
#include <pcl/visualization/cloud_viewer.h>

#include <opencv2/opencv.hpp>

#include <ros/ros.h>
#include <ros/spinner.h>
#include <sensor_msgs/CameraInfo.h>
#include <sensor_msgs/image_encodings.h>  // convert between opencv images and ros msgs
#include <sensor_msgs/Image.h>

#include <cv_bridge/cv_bridge.h>

#include <image_transport/image_transport.h>
#include <image_transport/subscriber_filter.h>

#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/exact_time.h>
#include <message_filters/sync_policies/approximate_time.h>

#include <kinect2_bridge/kinect2_definitions.h>

 using namespace std;
 using namespace cv;

/** Global variables */
//-- Note, either copy these two files from opencv/data/haarscascades to your current folder, or change these locations

String face_cascade_name = "haarcascade_frontalface_alt.xml";
String eyes_cascade_name = "haarcascade_eye_tree_eyeglasses.xml";
String Reye_cascade_name = "haarcascade_mcs_righteye.xml";
String Leye_cascade_name = "haarcascade_mcs_lefteye.xml";
String nose_cascade_name = "haarscascade_mcs_nose.xml";
CascadeClassifier face_cascade;
CascadeClassifier eyes_cascade;
CascadeClassifier nose_cascade;
CascadeClassifier Reye_cascade;
CascadeClassifier Leye_cascade;

RNG rng(12345);

//font properties
static const cv::Point pos(5, 15);
static const cv::Scalar colorText = CV_RGB(255, 255, 255);
static const double sizeText = 0.5;
static const int lineText = 1;
static const int font = cv::FONT_HERSHEY_SIMPLEX;

std::ostringstream oss;

/** Function Headers */
void detectAndDisplay( Mat detframe );
void reconstruct( Point eye_center );

class Receiver
{
public:
  enum Mode
  {
    IMAGE = 0,
    CLOUD,
    BOTH
  };

private:
  std::mutex lock;

  const std::string topicColor, topicDepth;
  const bool useExact, useCompressed;

  bool updateImage, updateCloud;
  bool save;
  bool running;
  size_t frame;
  const size_t queueSize;

  cv::Mat color, depth;
  cv::Mat cameraMatrixColor, cameraMatrixDepth;
  cv::Mat lookupX, lookupY;

  typedef message_filters::sync_policies::ExactTime<sensor_msgs::Image, sensor_msgs::Image, sensor_msgs::CameraInfo, sensor_msgs::CameraInfo> ExactSyncPolicy;
  typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::Image, sensor_msgs::Image, sensor_msgs::CameraInfo, sensor_msgs::CameraInfo> ApproximateSyncPolicy;

  ros::NodeHandle nh;             //starts the ROS cpp node, the 1st calls ros::start() and the last calls ros::shutdown()
  ros::AsyncSpinner spinner;
  image_transport::ImageTransport it;
  image_transport::SubscriberFilter *subImageColor, *subImageDepth;
  message_filters::Subscriber<sensor_msgs::CameraInfo> *subCameraInfoColor, *subCameraInfoDepth;

  message_filters::Synchronizer<ExactSyncPolicy> *syncExact;
  message_filters::Synchronizer<ApproximateSyncPolicy> *syncApproximate;

  std::thread imageViewerThread;
  Mode mode;

  pcl::PointCloud<pcl::PointXYZRGBA>::Ptr cloud;
  pcl::PCDWriter writer;
  //std::ostringstream oss;
  std::vector<int> params;

public:
  Receiver(const std::string &topicColor, const std::string &topicDepth, const bool useExact, const bool useCompressed)
    : topicColor(topicColor), topicDepth(topicDepth), useExact(useExact), useCompressed(useCompressed),
      updateImage(false), updateCloud(false), save(false), running(false), frame(0), queueSize(5),
      nh("~"), spinner(0), it(nh), mode(CLOUD)
  {
    cameraMatrixColor = cv::Mat::zeros(3, 3, CV_64F);
    cameraMatrixDepth = cv::Mat::zeros(3, 3, CV_64F);
    params.push_back(cv::IMWRITE_JPEG_QUALITY);
    params.push_back(100);
    params.push_back(cv::IMWRITE_PNG_COMPRESSION);
    params.push_back(1);
    params.push_back(cv::IMWRITE_PNG_STRATEGY);
    params.push_back(cv::IMWRITE_PNG_STRATEGY_RLE);
    params.push_back(0);
  }

  ~Receiver()
  {
  }

  void run(const Mode mode)
  {
    start(mode);
    stop();
  }

private:
  void start(const Mode mode)
  {
    this->mode = mode;
    running = true;

    std::string topicCameraInfoColor = topicColor.substr(0, topicColor.rfind('/')) + "/camera_info";
    std::string topicCameraInfoDepth = topicDepth.substr(0, topicDepth.rfind('/')) + "/camera_info";

    image_transport::TransportHints hints(useCompressed ? "compressed" : "raw");
    subImageColor = new image_transport::SubscriberFilter(it, topicColor, queueSize, hints);
    subImageDepth = new image_transport::SubscriberFilter(it, topicDepth, queueSize, hints);
    subCameraInfoColor = new message_filters::Subscriber<sensor_msgs::CameraInfo>(nh, topicCameraInfoColor, queueSize);
    subCameraInfoDepth = new message_filters::Subscriber<sensor_msgs::CameraInfo>(nh, topicCameraInfoDepth, queueSize);

    if(useExact)
    {
      syncExact = new message_filters::Synchronizer<ExactSyncPolicy>(ExactSyncPolicy(queueSize), *subImageColor, *subImageDepth, *subCameraInfoColor, *subCameraInfoDepth);
      syncExact->registerCallback(boost::bind(&Receiver::callback, this, _1, _2, _3, _4));
    }
    else
    {
      syncApproximate = new message_filters::Synchronizer<ApproximateSyncPolicy>(ApproximateSyncPolicy(queueSize), *subImageColor, *subImageDepth, *subCameraInfoColor, *subCameraInfoDepth);
      syncApproximate->registerCallback(boost::bind(&Receiver::callback, this, _1, _2, _3, _4));
    }

    spinner.start();

    std::chrono::milliseconds duration(1);
    while(!updateImage || !updateCloud)
    {
      if(!ros::ok())
      {
        return;
      }
      std::this_thread::sleep_for(duration);
    }
    cloud = pcl::PointCloud<pcl::PointXYZRGBA>::Ptr(new pcl::PointCloud<pcl::PointXYZRGBA>());
    cloud->height = color.rows;
    cloud->width = color.cols;
    cloud->is_dense = false;
    cloud->points.resize(cloud->height * cloud->width);
    createLookup(this->color.cols, this->color.rows);

    switch(mode)
    {
    case CLOUD:
      cloudViewer();
      break;
    case IMAGE:
      imageViewer();
      break;
    case BOTH:
      imageViewerThread = std::thread(&Receiver::imageViewer, this);
      cloudViewer();
      break;
    }
  }

  void stop()
  {
    spinner.stop();

    if(useExact)
    {
      delete syncExact;
    }
    else
    {
      delete syncApproximate;
    }

    delete subImageColor;
    delete subImageDepth;
    delete subCameraInfoColor;
    delete subCameraInfoDepth;

    running = false;
    if(mode == BOTH)
    {
      imageViewerThread.join();
    }
  }

  void callback(const sensor_msgs::Image::ConstPtr imageColor, const sensor_msgs::Image::ConstPtr imageDepth,
                const sensor_msgs::CameraInfo::ConstPtr cameraInfoColor, const sensor_msgs::CameraInfo::ConstPtr cameraInfoDepth)
  {
    cv::Mat color, depth;

    readCameraInfo(cameraInfoColor, cameraMatrixColor);
    readCameraInfo(cameraInfoDepth, cameraMatrixDepth);
    //cout<< "Color Matrix: " << cameraMatrixColor << endl;
    //cout<< "Depth Matrix: " << cameraMatrixDepth << endl;
    readImage(imageColor, color);
    readImage(imageDepth, depth);

    // IR image input
    if(color.type() == CV_16U)
    {
      cv::Mat tmp;
      color.convertTo(tmp, CV_8U, 0.02);
      cv::cvtColor(tmp, color, CV_GRAY2BGR);
    }

    lock.lock();
    this->color = color;
    this->depth = depth;
    updateImage = true;
    updateCloud = true;
    lock.unlock();
  }

  void imageViewer()
  {
    cv::Mat color, depth, depthDisp, combined, detframe;
    Point eye_center;
    std::chrono::time_point<std::chrono::high_resolution_clock> start, now;
    double fps = 0;
    size_t frameCount = 0;
    std::ostringstream oss;

    cv::namedWindow("Image Viewer");
    oss << "starting...";

    start = std::chrono::high_resolution_clock::now();
    for(; running && ros::ok();)
    {
      if(updateImage)
      {
        lock.lock();
        color = this->color;
        depth = this->depth;
        updateImage = false;
        lock.unlock();

        ++frameCount;
        now = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() / 1000.0;
        
        if(elapsed >= 1.0)
        {
          fps = frameCount / elapsed;
          oss.str("");
          oss << "fps: " << fps << " ( " << elapsed / frameCount * 1000.0 << " ms)";
          start = now;
          frameCount = 0;
        }

        dispDepth(depth, depthDisp, 12000.0f);
        combine(color, depthDisp, combined);

        //detect faces, eyes and nose
        detframe = combined.clone();   //create deep clone of image for the face detection
        detectAndDisplay( detframe );   //currently reduces native rate to 5.5Hz

        cv::putText(combined, oss.str(), pos, font, sizeText, colorText, lineText, CV_AA);
        cv::imshow("Image Viewer", combined);
      }

      int key = cv::waitKey(1);
      switch(key & 0xFF)
      {
      case 27:
      case 'q':
        running = false;
        break;
      case ' ':
      case 's':
        if(mode == IMAGE)
        {
          createCloud(depth, color, cloud);
          saveCloudAndImages(cloud, color, depth, depthDisp);
        }
        else
        {
          save = true;
        }
        break;
      }
      //delete projection;
    }
    cv::destroyAllWindows();
    cv::waitKey(100);
  }

  Point* peye_center = NULL;
  //peye_center = new Point;

  void detectAndDisplay( Mat detframe )
  {
    std::vector<Rect> faces;
    Mat frame_gray;

    cvtColor( detframe, frame_gray, COLOR_BGR2GRAY );
    equalizeHist( frame_gray, frame_gray );

    //load cascades
    face_cascade.load( face_cascade_name );
    eyes_cascade.load( eyes_cascade_name );
    nose_cascade.load( nose_cascade_name ); 
    Reye_cascade.load( Reye_cascade_name ); 
    Leye_cascade.load( Leye_cascade_name );

     //-- Detect faces
    face_cascade.detectMultiScale( frame_gray, faces, 1.1, 2, 0|CV_HAAR_SCALE_IMAGE, Size(30, 30) );

     #pragma omp parallel for 
    for( size_t i = 0; i < faces.size(); i++ )
    {
      Point vertex_one ( faces[i].x, faces[i].y);      
      Point vertex_two ( faces[i].x + faces[i].width, faces[i].y + faces[i].height);
      rectangle(detframe, vertex_one, vertex_two, Scalar(0, 255, 0), 2, 4, 0 );

      Mat faceROI = frame_gray( faces[i] );
      std::vector<Rect> eyes, noses;

    //-- In each face, detect eyes
      eyes_cascade.detectMultiScale( faceROI, eyes, 1.1, 2, 0 |CV_HAAR_SCALE_IMAGE, Size(30, 30) );
 
       #pragma omp parallel for 
      for( size_t j = 0; j < eyes.size(); j++ )
       {
        Point eye_center( faces[i].x + eyes[j].x + eyes[j].width/2, faces[i].y + eyes[j].y + eyes[j].height/2 );
        int radius = cvRound( (eyes[j].width + eyes[j].height)*0.25 );
        circle( detframe, eye_center, radius, Scalar( 255, 0, 0 ), 2, 8, 0 ); 
        circle( detframe, eye_center, 3.5, Scalar(255,255,255), CV_FILLED, 8, 0); 
        
        int eye_x = eye_center.x;        
        int eye_y = eye_center.y;    

        char textx[255], texty[255];
        sprintf(textx, "eye center, x (mm): %d", eye_x);
        sprintf(texty, "eye center, y (mm): %d", eye_y); 
        putText(detframe, textx, Point(5,35), font, sizeText, colorText, lineText,CV_AA);
        putText(detframe, texty, Point(5,55), font, sizeText, colorText, lineText, CV_AA); 
        reconstruct(eye_x, eye_y);        
      } 
    }
   cv::imshow( "Face and Features Viewer", detframe );
  }

  cv::Mat distortion      = cv::Mat::zeros(5, 1, CV_64F);
  cv::Mat scalefactor     = cv::Mat::zeros(1, 1, CV_64F);  
  cv::Mat scalecomps      = cv::Mat::zeros(1, 3, CV_64F);
  cv::Mat rotation        = cv::Mat::eye(3, 3, CV_64F);  
  cv::Mat translation     = cv::Mat::zeros(3, 1, CV_64F);
  cv:: Mat pf             = cv::Mat::zeros(3, 1, CV_64F);  
  cv::Mat camcenter       = cv::Mat::zeros(3, 1, CV_64F);
  cv::Mat projection      = cv::Mat::zeros(3, 4, CV_64F);     //[r1, r2, r3, t]
  cv::Mat homocat         = cv::Mat::zeros(3, 3, CV_64F);     //[r1 , r2, t]
  cv::Mat homographyraw   = cv::Mat::zeros(3, 3, CV_64F);     //K * [r1, r2, t]  
  cv::Mat homography      = cv::Mat::zeros(3, 3, CV_64F);     //K * [r1, r2, t]/t
  cv::Mat pixelpts        = cv::Mat::ones(3,1, CV_64F);      
  cv::Mat reconstructed   = cv::Mat::ones(3,1, CV_64F);  
  double s; 
  int xprime, yprime;

  void reconstruct(int eye_x, int eye_y)
  {
    //intrinsic parameters
    const float fx = cameraMatrixColor.at<double>(0, 0);
    const float fy = cameraMatrixColor.at<double>(1, 1);
    const float cx = cameraMatrixColor.at<double>(0, 2);
    const float cy = cameraMatrixColor.at<double>(1, 2);  

    //Couldn't figure a better way to retrieve the params from kinect2_bridge :(
    distortion.at<double >(0, 0) = 0.02732778206941041;
    distortion.at<double >(1, 0) = 0.06919310914717383;
    distortion.at<double >(2, 0) = -0.00305523856741313;
    distortion.at<double >(3, 0) = -0.003444061483684894;
    distortion.at<double >(4, 0) = -0.07593134286172079;

    const float k1 = distortion.at<double >(0, 0);
    const float k2 = distortion.at<double >(1, 0);
    const float p1 = distortion.at<double >(2, 0);
    const float p2 = distortion.at<double >(3, 0);
    const float k3 = distortion.at<double >(4, 0);

    rotation.at<double >(0, 0) = 0.9999839890693748;
    rotation.at<double >(0, 1) = -0.00220878479974752;
    rotation.at<double >(0, 2) = 0.005209882398764278;
    rotation.at<double >(1, 0) = 0.002169762562952003;
    rotation.at<double >(1, 1) = 0.9999696416803922;
    rotation.at<double >(1, 2) = 0.007483839122310252;
    rotation.at<double >(2, 0) = -0.005226254425586405;
    rotation.at<double >(2, 1) = -0.007472415091295038;
    rotation.at<double >(2, 2) = 0.9999584237743999;

    translation.at<double >(0, 0) = -0.04598755491059946;
    translation.at<double >(1, 0) = 9.878938204711128e-05;
    translation.at<double >(2, 0) = 0.005470134429191416;

    //8.1, p.196, Zisserman and Hartley
    homocat.at<double >(0, 0) = rotation.at<double >(0, 0);
    homocat.at<double >(0, 1) = rotation.at<double >(0, 1);
    homocat.at<double >(0, 2) = translation.at<double >(0, 0);
    homocat.at<double >(1, 0) = rotation.at<double >(1, 0);
    homocat.at<double >(1, 1) = rotation.at<double >(1, 1);
    homocat.at<double >(1, 2) = translation.at<double >(1, 0);
    homocat.at<double >(2, 0) = rotation.at<double >(2, 0);
    homocat.at<double >(2, 1) = rotation.at<double >(2, 1);
    homocat.at<double >(2, 2) = translation.at<double >(2, 0);

    projection.at<double >(0, 0) = 526.33795064532;
    projection.at<double >(0, 1) = 0;
    projection.at<double >(0, 2) = 478.4995813884854;
    projection.at<double >(0, 3) = 0;
    projection.at<double >(1, 0) = 0;
    projection.at<double >(1, 1) = 526.6946594095425;
    projection.at<double >(1, 2) = 263.8883319922702;
    projection.at<double >(1, 3) = 0;
    projection.at<double >(2, 0) = 0;
    projection.at<double >(2, 1) = 0;
    projection.at<double >(2, 2) = 1;
    projection.at<double >(2, 3) = 0;   

    homographyraw             = cameraMatrixColor * homocat;
    homography                = homographyraw / translation.at<double>(2, 0);

    const float r       = pow( (eye_x - cx), 2) + pow( (eye_y -cy), 2) ;            // radial distance from center
    const float inner   = 1               + (k1 * r)           + (k2 * pow(r,2))  + (k3 * pow(r, 3));
    const float xprime  = (eye_x * inner) + (2 * p1 * eye_x * eye_y) + p2 * (r + 2 * pow(eye_x, 2) );   //xprime is my x''
    const float yprime  = (eye_y * inner) + (2 * p2 * eye_x * eye_y) + p1 * (r + 2 * pow(eye_y, 2) );   //yprime is my y''

    const float ux = floor (fx * xprime + cx);   
    const float vy = floor (fy * yprime + cy);   //yprime is my y''

    //convert u,v points to pixel ints
    int u = (int) floor(ux + 0.5); 
    int v = (int) floor(vy + 0.5);

    //These are the values after accounting for radial distortion and tangential distortion
    pixelpts.at<int>(0,0) = u;
    pixelpts.at<int>(1,0) = v;

    //compute scale factor, k
    scalecomps.at<double>(0, 0) = homocat.at<double >(2, 0);
    scalecomps.at<double>(0, 1) = homocat.at<double >(2, 1);
    scalecomps.at<double>(0, 2) = homocat.at<double >(2, 2);

    scalefactor = scalecomps * pixelpts;                //s or k as you might call it

    //last column of projection
    pf.at<double >(0, 3) = projection.at<double >(0, 3);
    pf.at<double >(1, 3) = projection.at<double >(1, 3);
    pf.at<double >(2, 3) = projection.at<double >(2, 3);

    Mat M = cameraMatrixColor * rotation ;            //M = KR
    camcenter = -1.0 * M.inv() * pf;              //Find camera center

    reconstructed = M.inv() * (pixelpts - pf );
    cout <<"reconstruction = " << reconstructed << endl;

/*
    //project ::http://stackoverflow.com/questions/7836134/get-3d-coord-from-2d-image-pixel-if-we-know-extrinsic-and-intrinsic-parameters/10750648#10750648
    reconstructed = homography * pixelpts;

    cout <<"world coordinates = " << reconstructed << endl;*/
  }
  
  void cloudViewer()
  {
    cv::Mat color, depth;
    pcl::visualization::PCLVisualizer::Ptr visualizer(new pcl::visualization::PCLVisualizer("Cloud Viewer"));
    const std::string cloudName = "rendered";

    lock.lock();
    color = this->color;
    depth = this->depth;
    updateCloud = false;
    lock.unlock();

    createCloud(depth, color, cloud);

    visualizer->addPointCloud(cloud, cloudName);
    visualizer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 1, cloudName);
    visualizer->initCameraParameters();
    visualizer->setBackgroundColor(0, 0, 0);
    visualizer->setPosition(mode == BOTH ? color.cols : 0, 0);
    visualizer->setSize(color.cols, color.rows);
    visualizer->setShowFPS(true);
    visualizer->setCameraPosition(0, 0, 0, 0, -1, 0);
    visualizer->registerKeyboardCallback(&Receiver::keyboardEvent, *this);

    for(; running && ros::ok();)
    {
      if(updateCloud)
      {
        lock.lock();
        color = this->color;
        depth = this->depth;
        updateCloud = false;
        lock.unlock();

        createCloud(depth, color, cloud);

        visualizer->updatePointCloud(cloud, cloudName);
      }
      if(save)
      {
        save = false;
        cv::Mat depthDisp;
        dispDepth(depth, depthDisp, 12000.0f);
        saveCloudAndImages(cloud, color, depth, depthDisp);
      }
      visualizer->spinOnce(10);
    }
    visualizer->close();
  }

  void keyboardEvent(const pcl::visualization::KeyboardEvent &event, void *)
  {
    if(event.keyUp())
    {
      switch(event.getKeyCode())
      {
      case 27:
      case 'q':
        running = false;
        break;
      case ' ':
      case 's':
        save = true;
        break;
      }
    }
  }

  void readImage(const sensor_msgs::Image::ConstPtr msgImage, cv::Mat &image) const
  {
    cv_bridge::CvImageConstPtr pCvImage;
    pCvImage = cv_bridge::toCvShare(msgImage, msgImage->encoding);
    pCvImage->image.copyTo(image);
  }

  void readCameraInfo(const sensor_msgs::CameraInfo::ConstPtr cameraInfo, cv::Mat &cameraMatrix) const
  {
    double *itC = cameraMatrix.ptr<double>(0, 0);
    for(size_t i = 0; i < 9; ++i, ++itC)
    {
      *itC = cameraInfo->K[i];
    }
  }

  void dispDepth(const cv::Mat &in, cv::Mat &out, const float maxValue)
  {
    cv::Mat tmp = cv::Mat(in.rows, in.cols, CV_8U);
    const uint32_t maxInt = 255;

    #pragma omp parallel for
    for(int r = 0; r < in.rows; ++r)
    {
      const uint16_t *itI = in.ptr<uint16_t>(r);
      uint8_t *itO = tmp.ptr<uint8_t>(r);

      for(int c = 0; c < in.cols; ++c, ++itI, ++itO)
      {
        *itO = (uint8_t)std::min((*itI * maxInt / maxValue), 255.0f);
      }
    }

    cv::applyColorMap(tmp, out, cv::COLORMAP_JET);
  }

  void combine(const cv::Mat &inC, const cv::Mat &inD, cv::Mat &out)
  {
    out = cv::Mat(inC.rows, inC.cols, CV_8UC3);

    #pragma omp parallel for //openmp parallelization so you don't have to rewrite existing code.
    for(int r = 0; r < inC.rows; ++r)
    {
      const cv::Vec3b
      *itC = inC.ptr<cv::Vec3b>(r),
      *itD = inD.ptr<cv::Vec3b>(r);
      cv::Vec3b *itO = out.ptr<cv::Vec3b>(r);

      for(int c = 0; c < inC.cols; ++c, ++itC, ++itD, ++itO)
      {
        itO->val[0] = (itC->val[0] + itD->val[0]) >> 1;
        itO->val[1] = (itC->val[1] + itD->val[1]) >> 1;
        itO->val[2] = (itC->val[2] + itD->val[2]) >> 1;
      }
    }
  }

  void createCloud(const cv::Mat &depth, const cv::Mat &color, pcl::PointCloud<pcl::PointXYZRGBA>::Ptr &cloud) const
  {
    const float badPoint = std::numeric_limits<float>::quiet_NaN();

    #pragma omp parallel for
    for(int r = 0; r < depth.rows; ++r)
    {
      pcl::PointXYZRGBA *itP = &cloud->points[r * depth.cols];
      const uint16_t *itD = depth.ptr<uint16_t>(r);
      const cv::Vec3b *itC = color.ptr<cv::Vec3b>(r);
      const float y = lookupY.at<float>(0, r);
      const float *itX = lookupX.ptr<float>();

      for(size_t c = 0; c < (size_t)depth.cols; ++c, ++itP, ++itD, ++itC, ++itX)
      {
        register const float depthValue = *itD / 1000.0f;
        // Check for invalid measurements
        if(isnan(depthValue) || depthValue <= 0.001)
        {
          // not valid
          itP->x = itP->y = itP->z = badPoint;
          itP->rgba = 0;
          continue;
        }
        itP->z = depthValue;
        itP->x = *itX * depthValue;
        itP->y = y * depthValue;
        itP->b = itC->val[0];
        itP->g = itC->val[1];
        itP->r = itC->val[2];
        itP->a = 255;
      }
      cout << "itP->z: " << itP->z << endl;
    }
  }

  void saveCloudAndImages(const pcl::PointCloud<pcl::PointXYZRGBA>::ConstPtr cloud, const cv::Mat &color, const cv::Mat &depth, const cv::Mat &depthColored)
  {
    oss.str("");
    oss << "./" << std::setfill('0') << std::setw(4) << frame;
    const std::string baseName = oss.str();
    const std::string cloudName = baseName + "_cloud.pcd";
    const std::string colorName = baseName + "_color.jpg";
    const std::string depthName = baseName + "_depth.png";
    const std::string depthColoredName = baseName + "_depth_colored.png";

    std::cout << "saving cloud: " << cloudName << std::endl;
    writer.writeBinary(cloudName, *cloud);
    std::cout << "saving color: " << colorName << std::endl;
    cv::imwrite(colorName, color, params);
    std::cout << "saving depth: " << depthName << std::endl;
    cv::imwrite(depthName, depth, params);
    std::cout << "saving depth: " << depthColoredName << std::endl;
    cv::imwrite(depthColoredName, depthColored, params);
    std::cout << "saving complete!" << std::endl;
    ++frame;
  }

  void createLookup(size_t width, size_t height)
  {
    const float fx = 1.0f / cameraMatrixColor.at<double>(0, 0);
    const float fy = 1.0f / cameraMatrixColor.at<double>(1, 1);
    const float cx = cameraMatrixColor.at<double>(0, 2);
    const float cy = cameraMatrixColor.at<double>(1, 2);   
    float *it;

    lookupY = cv::Mat(1, height, CV_32F);
    it = lookupY.ptr<float>();
    for(size_t r = 0; r < height; ++r, ++it)
    {
      *it = (r - cy) * fy;
    }

    lookupX = cv::Mat(1, width, CV_32F);
    it = lookupX.ptr<float>();
    for(size_t c = 0; c < width; ++c, ++it)
    {
      *it = (c - cx) * fx;
    }
  }
};

void help(const std::string &path)
{
  std::cout << path << " [options]" << std::endl
            << "         name: 'any string' equals to the kinect2_bridge topic base name" << std::endl
            << "         mode: 'qhd', 'hd', 'sd' or 'ir'" << std::endl
            << "         visualization: 'image', 'cloud' or 'both'" << std::endl
            << "         options:" << std::endl
            << "         'compressed' use compressed instead of raw topics" << std::endl
            << "         'approx' use approximate time synchronization" << std::endl;
}

int main(int argc, char **argv)
{
  ros::init(argc, argv, "kinect2_viewer", ros::init_options::AnonymousName);

  if(!ros::ok())
  {
    return 0;
  }

  std::string ns = K2_DEFAULT_NS;
  std::string topicColor = K2_TOPIC_QHD K2_TOPIC_IMAGE_COLOR K2_TOPIC_IMAGE_RECT;
  std::string topicDepth = K2_TOPIC_QHD K2_TOPIC_IMAGE_DEPTH K2_TOPIC_IMAGE_RECT;
  bool useExact = true;
  bool useCompressed = false;
  Receiver::Mode mode = Receiver::CLOUD;

  for(size_t i = 1; i < (size_t)argc; ++i)
  {
    std::string param(argv[i]);

    if(param == "-h" || param == "--help" || param == "-?" || param == "--?")
    {
      help(argv[0]);
      ros::shutdown();
      return 0;
    }
    else if(param == "qhd")
    {
      topicColor = K2_TOPIC_QHD K2_TOPIC_IMAGE_COLOR K2_TOPIC_IMAGE_RECT;
      topicDepth = K2_TOPIC_QHD K2_TOPIC_IMAGE_DEPTH K2_TOPIC_IMAGE_RECT;
    }
    
    else if(param == "hd")
    {
      topicColor = K2_TOPIC_HD K2_TOPIC_IMAGE_COLOR K2_TOPIC_IMAGE_RECT;
      topicDepth = K2_TOPIC_HD K2_TOPIC_IMAGE_DEPTH K2_TOPIC_IMAGE_RECT;
    }
    else if(param == "ir")
    {
      topicColor = K2_TOPIC_SD K2_TOPIC_IMAGE_IR K2_TOPIC_IMAGE_RECT;
      topicDepth = K2_TOPIC_SD K2_TOPIC_IMAGE_DEPTH K2_TOPIC_IMAGE_RECT;      
    }
    else if(param == "sd")
    {
      topicColor = K2_TOPIC_SD K2_TOPIC_IMAGE_COLOR K2_TOPIC_IMAGE_RECT;
      topicDepth = K2_TOPIC_SD K2_TOPIC_IMAGE_DEPTH K2_TOPIC_IMAGE_RECT;
    }
    else if(param == "approx")
    {
      useExact = false;
    }

    else if(param == "compressed")
    {
      useCompressed = true;
    }
    else if(param == "image")
    {
      mode = Receiver::IMAGE;
    }
    else if(param == "cloud")
    {
      mode = Receiver::CLOUD;
    }
    else if(param == "both")
    {
      mode = Receiver::BOTH;
    }
    else
    {
      ns = param;
    }
  }

  topicColor = "/" + ns + topicColor;
  topicDepth = "/" + ns + topicDepth;
  std::cout << "topic color: " << topicColor << std::endl;
  std::cout << "topic depth: " << topicDepth << std::endl;

  Receiver receiver(topicColor, topicDepth, useExact, useCompressed);

  std::cout << "starting receiver..." << std::endl;
  receiver.run(mode);
  ros::shutdown();
  return 0;
}

