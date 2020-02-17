#include "kinect_sub/kinect_sub.h"
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/image_encodings.h>
#include <image_geometry/pinhole_camera_model.h>

KinectSub::KinectSub(const std::function<void(cv::Mat, cv::Mat, cv::Matx33d)>& _externCallback,
                     const SubscriptionOptions _options)
    : externCallback(_externCallback)
    , options(_options)
    , callbackQueue()
    , spinner(1, &callbackQueue)
{
    options.nh.setCallbackQueue(&callbackQueue);
    options.pnh.setCallbackQueue(&callbackQueue);

    it = std::make_unique<image_transport::ImageTransport>(options.nh);
    rgb_sub = std::make_unique<image_transport::SubscriberFilter>(*it, options.rgb_topic, options.queue_size, options.hints);
    depth_sub = std::make_unique<image_transport::SubscriberFilter>(*it, options.depth_topic, options.queue_size, options.hints);
    cam_sub = std::make_unique<message_filters::Subscriber<sensor_msgs::CameraInfo>>(options.nh, options.cam_topic, options.queue_size);

    sync = std::make_unique<message_filters::Synchronizer<SyncPolicy>>(SyncPolicy(options.queue_size), *rgb_sub, *depth_sub, *cam_sub);
    sync->registerCallback(boost::bind(&KinectSub::imageCb, this, _1, _2, _3));

    spinner.start();
}

void KinectSub::imageCb(const sensor_msgs::ImageConstPtr& rgb_msg,
                        const sensor_msgs::ImageConstPtr& depth_msg,
                        const sensor_msgs::CameraInfoConstPtr& cam_msg)
{
    cv_bridge::CvImagePtr cv_rgb_ptr;
    try
    {
        cv_rgb_ptr = cv_bridge::toCvCopy(rgb_msg, sensor_msgs::image_encodings::BGR8);
    }
    catch (cv_bridge::Exception& e)
    {
        ROS_ERROR("cv_bridge exception: %s", e.what());
        return;
    }

    cv_bridge::CvImagePtr cv_depth_ptr;
    if ("16UC1" == depth_msg->encoding)
    {
        try
        {
            cv_depth_ptr = cv_bridge::toCvCopy(depth_msg, sensor_msgs::image_encodings::TYPE_16UC1); // MONO16?
        }
        catch (cv_bridge::Exception& e)
        {
            ROS_ERROR("cv_bridge exception: %s", e.what());
            return;
        }
    }
    else if ("32FC1" == depth_msg->encoding)
    {
        try
        {
            cv_depth_ptr = cv_bridge::toCvCopy(depth_msg, sensor_msgs::image_encodings::TYPE_32FC1);
        }
        catch (cv_bridge::Exception& e)
        {
            ROS_ERROR("cv_bridge exception: %s", e.what());
            return;
        }

        cv::Mat convertedDepthImg(cv_depth_ptr->image.size(), CV_16UC1);

        const int V = cv_depth_ptr->image.size().height;
        const int U = cv_depth_ptr->image.size().width;

        #pragma omp parallel for
        for (int v = 0; v < V; ++v)
        {
            for (int u = 0; u < U; ++u)
            {
                convertedDepthImg.at<uint16_t>(v, u) =
                        depth_image_proc::DepthTraits<uint16_t>::fromMeters(cv_depth_ptr->image.at<float>(v, u));
            }
        }

        cv_depth_ptr->encoding = "16UC1";
        cv_depth_ptr->image = convertedDepthImg;
    }

    image_geometry::PinholeCameraModel cameraModel;
    cameraModel.fromCameraInfo(cam_msg);

    // TODO: make the extern callback take the camera info as well
    if (externCallback)
    {
        externCallback(cv_rgb_ptr->image, cv_depth_ptr->image, cameraModel.fullIntrinsicMatrix());
    }
}

