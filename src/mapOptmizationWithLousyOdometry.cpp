#include "utility.h"
#include "kloam/cloud_info.h"
#include "kloam/save_map.h"

#include <gtsam/geometry/Rot3.h>
#include <gtsam/geometry/Pose3.h>
#include <gtsam/slam/PriorFactor.h>
#include <gtsam/slam/BetweenFactor.h>
#include <gtsam/navigation/GPSFactor.h>
#include <gtsam/navigation/ImuFactor.h>
#include <gtsam/navigation/CombinedImuFactor.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/nonlinear/Marginals.h>
#include <gtsam/nonlinear/Values.h>
#include <gtsam/inference/Symbol.h>

#include <gtsam/nonlinear/ISAM2.h>
#include"Scancontext.h"
#include <geometry_msgs/PoseWithCovarianceStamped.h>

using namespace gtsam;

using symbol_shorthand::X; // Pose3 (x,y,z,r,p,y)
using symbol_shorthand::V; // Vel   (xdot,ydot,zdot)
using symbol_shorthand::B; // Bias  (ax,ay,az,gx,gy,gz)
using symbol_shorthand::G; // GPS pose
/*
    * A point cloud type that has 6D pose info ([x,y,z,roll,pitch,yaw] intensity is time stamp)
    */

struct PointXYZIRPYT
{
    PCL_ADD_POINT4D
    PCL_ADD_INTENSITY;                  // preferred way of adding a XYZ+padding
    float roll;
    float pitch;
    float yaw;
    double time;
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW   // make sure our new allocators are aligned
} EIGEN_ALIGN16;                    // enforce SSE padding for correct memory alignment

POINT_CLOUD_REGISTER_POINT_STRUCT (PointXYZIRPYT,
                                   (float, x, x) (float, y, y)
                                   (float, z, z) (float, intensity, intensity)
                                   (float, roll, roll) (float, pitch, pitch) (float, yaw, yaw)
                                   (double, time, time))

typedef PointXYZIRPYT  PointTypePose;
typedef geometry_msgs::PoseWithCovarianceStampedConstPtr rvizPoseType;

class mapOptimization : public ParamServer
{

public:
    //gps
    double rns;
    double rew;
    bool relocSuccess = false;
    bool mapLoaded = false;

    vector<nav_msgs::Odometry::ConstPtr> gtVec;
    // for temporary mapping mode
    double rosTimeStart = -1;
    bool temporaryMappingMode = false;
    float transformBeforeMapped[6];
    bool goodToMergeMap = false;
    int startTemporaryMappingIndex = -1;
    float mergeNoise;
    bool frameTobeAbandoned = false;
    bool poorlyMatched = false;
    int TMMcount = 0;

    // // use intensity channel of K pointclouds to record the error counts: K=5 for now
    // int K = 5;
    // vector<pcl::PointCloud<PointType>> errorCounts; // not the best way to do this 

    // data analysis
    int iterCount = 0;
    vector<float> mappingTimeVec;
    int edgePointCorrNum = 0;
    int surfPointCorrNum = 0;
    pcl::PointCloud<PointType>::Ptr edgeErrorCloud;
    pcl::PointCloud<PointType>::Ptr surfErrorCloud;
    pcl::PointCloud<PointType>::Ptr tmpEdgeErrorCloud;
    pcl::PointCloud<PointType>::Ptr tmpSurfErrorCloud;
    float maxEdgeIntensity = -1;
    float maxSurfIntensity = -1;
    float maxIntensity = -1;

    Eigen::Affine3f mergeCorrection;

    pcl::PointCloud<PointType>::Ptr keyPosesTarget3D;
    pcl::PointCloud<PointTypePose>::Ptr keyPosesTarget6D;

    // for kitti pose save
    Eigen::Affine3f H_init;
    vector<Eigen::Affine3f> pose_kitti_vec;

    bool doneSavingMap = false;

    Eigen::Affine3f affine_lidar_to_odom; // convert points in lidar frame to odom frame
    Eigen::Affine3f affine_lidar_to_map;
    Eigen::Affine3f affine_odom_to_map;

    vector<double> mapRegistrationError;
    vector<vector<double>> mappingLogs;
    vector<float> noiseVec;
    pcl::PointCloud<PointType>::Ptr submap;
    vector<rvizPoseType> poseEstVec;
    Eigen::Affine3f relocCorrection;

    // gtsam
    NonlinearFactorGraph gtSAMgraph;
    Values initialEstimate;
    Values optimizedEstimate;
    ISAM2 *isam;
    Values isamCurrentEstimate;
    Eigen::MatrixXd poseCovariance;

    ros::Publisher pubLidarCloudSurround;
    ros::Publisher pubLidarOdometryGlobal;
    ros::Publisher pubLidarOdometryGlobalFusion;
    ros::Publisher pubKeyPoses;
    ros::Publisher pubPath;
    ros::Publisher pubPathFusion;

    ros::Publisher pubHistoryKeyFrames;
    ros::Publisher pubIcpKeyFrames;
    ros::Publisher pubRecentKeyFrames;
    ros::Publisher pubRecentKeyFrame;
    ros::Publisher pubCloudRegisteredRaw;
    ros::Publisher pubLoopConstraintEdge;
    ros::Publisher pubKeyPosesTmp;

    ros::Subscriber subCloud;
    ros::Subscriber subGPS;
    ros::Subscriber subGT;
    ros::Subscriber subLoop;
    ros::Subscriber subLidarOdometry;
    ros::Subscriber initialpose_sub;
    ros::ServiceServer srvSaveMap;

    std::deque<nav_msgs::Odometry> gpsQueue;
    vector<nav_msgs::Odometry> gpsVec;

    kloam::cloud_info cloudInfo;
    queue<kloam::cloud_infoConstPtr> cloudInfoBuffer;
    queue<nav_msgs::Odometry::ConstPtr> lidarOdometryBuffer;

    vector<pcl::PointCloud<PointType>::Ptr> cornerCloudKeyFrames;
    vector<pcl::PointCloud<PointType>::Ptr> surfCloudKeyFrames;

    vector<pcl::PointCloud<PointType>::Ptr> temporaryCornerCloudKeyFrames;
    vector<pcl::PointCloud<PointType>::Ptr> temporarySurfCloudKeyFrames;
    
    pcl::PointCloud<PointType>::Ptr cloudKeyPoses3D;
    pcl::PointCloud<PointTypePose>::Ptr cloudKeyPoses6D;

    pcl::PointCloud<PointType>::Ptr copy_cloudKeyPoses3D;
    pcl::PointCloud<PointTypePose>::Ptr copy_cloudKeyPoses6D;

    pcl::PointCloud<PointType>::Ptr temporaryCloudKeyPoses3D;
    pcl::PointCloud<PointTypePose>::Ptr temporaryCloudKeyPoses6D;


    pcl::PointCloud<PointType>::Ptr lidarCloudCornerLast; // corner feature set from odoOptimization
    pcl::PointCloud<PointType>::Ptr lidarCloudSurfLast; // surf feature set from odoOptimization
    pcl::PointCloud<PointType>::Ptr lidarCloudCornerLastDS; // downsampled corner featuer set from odoOptimization
    pcl::PointCloud<PointType>::Ptr lidarCloudSurfLastDS; // downsampled surf featuer set from odoOptimization

    pcl::PointCloud<PointType>::Ptr lidarCloudOri;
    pcl::PointCloud<PointType>::Ptr coeffSel;

    std::vector<PointType> lidarCloudOriCornerVec; // corner point holder for parallel computation
    std::vector<PointType> coeffSelCornerVec;
    std::vector<bool> lidarCloudOriCornerFlag;
    std::vector<PointType> lidarCloudOriSurfVec; // surf point holder for parallel computation
    std::vector<PointType> coeffSelSurfVec;
    std::vector<bool> lidarCloudOriSurfFlag;

    map<int, pair<pcl::PointCloud<PointType>, pcl::PointCloud<PointType>>> lidarCloudMapContainer;
    pcl::PointCloud<PointType>::Ptr lidarCloudCornerFromMap;
    pcl::PointCloud<PointType>::Ptr lidarCloudSurfFromMap;
    pcl::PointCloud<PointType>::Ptr lidarCloudCornerFromMapDS;
    pcl::PointCloud<PointType>::Ptr lidarCloudSurfFromMapDS;

    pcl::KdTreeFLANN<PointType>::Ptr kdtreeCornerFromMap;
    pcl::KdTreeFLANN<PointType>::Ptr kdtreeSurfFromMap;

    pcl::KdTreeFLANN<PointType>::Ptr kdtreeSurroundingKeyPoses;
    pcl::KdTreeFLANN<PointType>::Ptr kdtreeHistoryKeyPoses;

    pcl::VoxelGrid<PointType> downSizeFilterCorner;
    pcl::VoxelGrid<PointType> downSizeFilterSurf;
    pcl::VoxelGrid<PointType> downSizeFilterICP;
    pcl::VoxelGrid<PointType> downSizeFilterSurroundingKeyPoses; // for surrounding key poses of scan-to-map optimization
    pcl::VoxelGrid<PointType> downSizeFilterSavingKeyframes; // for surrounding key poses of scan-to-map optimization
    
    ros::Time timeLidarInfoStamp;
    double cloudInfoTime;

    float odometryError;
    float transformTobeMapped[6];
    
    float lidarRollInit, lidarPitchInit,lidarYawInit;
    std::mutex mtx;
    std::mutex mtxInit;
    std::mutex mtxLoopInfo;
    std::mutex pose_estimator_mutex;
    // std::mutext mtxReloc;
    bool isDegenerate = false;
    cv::Mat matP;

    int lidarCloudCornerFromMapDSNum = 0;
    int lidarCloudSurfFromMapDSNum = 0;
    int lidarCloudCornerLastDSNum = 0;
    int lidarCloudSurfLastDSNum = 0;

    bool aLoopIsClosed = false;
    
    multimap<int,int>    loopIndexContainer;
    vector<pair<int, int>> loopIndexQueue;
    vector<gtsam::Pose3> loopPoseQueue;
    vector<gtsam::noiseModel::Diagonal::shared_ptr> loopNoiseQueue;
    
    deque<std_msgs::Float64MultiArray> loopInfoVec;
    vector<nav_msgs::Odometry> globalOdometry;
    
    nav_msgs::Path globalPath;
    nav_msgs::Path globalPathFusion;


    bool poseGuessFromRvizAvailable = false;
    float rvizGuess[6];

    
    pcl::PointCloud<PointType>::Ptr lidarCloudRaw; 

    mapOptimization()
    {
        ISAM2Params parameters;
        parameters.relinearizeThreshold = 0.1;
        parameters.relinearizeSkip = 1;
        isam = new ISAM2(parameters);

        initialpose_sub = nh.subscribe("/initialpose", 1, &mapOptimization::initialpose_callback, this);

        pubKeyPosesTmp                 = nh.advertise<sensor_msgs::PointCloud2>("/kloam/mapping/tmp_key_poses", 1);
        pubKeyPoses                 = nh.advertise<sensor_msgs::PointCloud2>("/kloam/mapping/key_poses", 1);
        pubLidarCloudSurround       = nh.advertise<sensor_msgs::PointCloud2>("/kloam/mapping/map_global", 1);
        pubLidarOdometryGlobal      = nh.advertise<nav_msgs::Odometry> ("/kloam/mapping/odometry", 1);
        pubLidarOdometryGlobalFusion      = nh.advertise<nav_msgs::Odometry> ("/kloam/mapping/odometry_fusion", 1);
        pubPath                     = nh.advertise<nav_msgs::Path>("/kloam/mapping/path", 1);
        pubPathFusion               = nh.advertise<nav_msgs::Path>("/kloam/mapping/path_fusion", 1);

        pubHistoryKeyFrames   = nh.advertise<sensor_msgs::PointCloud2>("/kloam/mapping/icp_loop_closure_history_cloud", 1);
        pubIcpKeyFrames       = nh.advertise<sensor_msgs::PointCloud2>("/kloam/mapping/icp_loop_closure_corrected_cloud", 1);
        pubLoopConstraintEdge = nh.advertise<visualization_msgs::MarkerArray>("/kloam/mapping/loop_closure_constraints", 1);

        pubRecentKeyFrames    = nh.advertise<sensor_msgs::PointCloud2>("/kloam/mapping/map_local", 1);
        pubRecentKeyFrame     = nh.advertise<sensor_msgs::PointCloud2>("/kloam/mapping/cloud_registered", 1);
        pubCloudRegisteredRaw = nh.advertise<sensor_msgs::PointCloud2>("/kloam/mapping/cloud_registered_raw", 1);

        subCloud = nh.subscribe<kloam::cloud_info>("/kloam/feature/cloud_info", 10, &mapOptimization::lidarCloudInfoHandler, this);
        subGPS   = nh.subscribe<sensor_msgs::NavSatFix> (gpsTopic, 200, &mapOptimization::gpsHandler, this);
        subGT   = nh.subscribe<nav_msgs::Odometry> (gtTopic, 200, &mapOptimization::gtHandler, this); 
        subLidarOdometry = nh.subscribe<nav_msgs::Odometry> ("/kloam/lidarOdometry/laser_odom_to_init", 10, &mapOptimization::lidarOdometryHandler,this);

        srvSaveMap  = nh.advertiseService("/kloam/save_map", &mapOptimization::saveMapService, this);

        downSizeFilterCorner.setLeafSize(mappingCornerLeafSize, mappingCornerLeafSize, mappingCornerLeafSize);
        downSizeFilterSurf.setLeafSize(mappingSurfLeafSize, mappingSurfLeafSize, mappingSurfLeafSize);
        downSizeFilterICP.setLeafSize(mappingSurfLeafSize, mappingSurfLeafSize, mappingSurfLeafSize);
        downSizeFilterSurroundingKeyPoses.setLeafSize(surroundingKeyframeDensity, surroundingKeyframeDensity, surroundingKeyframeDensity); // for surrounding key poses of scan-to-map optimization

        // gps parameter calculation
        double earthEqu = 6378135;
        double earthPolar = 6356750;
        double tmp = sqrt(earthEqu*earthEqu*cos(deg2rad(lati0))*cos(deg2rad(lati0)) +earthPolar*earthPolar*sin(deg2rad(lati0))*sin(deg2rad(lati0)));
        rns = earthEqu*earthEqu*earthPolar*earthPolar/tmp/tmp/tmp;
        rew = earthEqu*earthEqu/tmp;

        allocateMemory();

        if (localizationMode)
        { // even ctrl+C won't terminate loading process
            std::lock_guard<std::mutex> lock(mtxInit);
            // load keyframe map
            if (!mapLoaded){
                ROS_INFO("************************loading keyframe map************************");
                string filePath = loadKeyframeMapDirectory+"/poses.txt";
                ifstream fin(filePath);
                if (!fin.is_open()) {
                    cout<<filePath<<" is not valid!"<<endl;
                    }
                while (true){
                    PointTypePose point;
                    PointType point3;
                    fin>>point.x>>point.y>>point.z>>point.roll>>point.pitch>>point.yaw>>point.intensity;
                    point3.x=point.x;
                    point3.y=point.y;
                    point3.z=point.z;
                    point3.intensity=point.intensity;                    
                    if(fin.peek()==EOF){ 
                        break;
                    }
                    else{
                        cloudKeyPoses6D->push_back(point);
                        cloudKeyPoses3D->push_back(point3);
                    }
                }
                int keyframeN = (int)cloudKeyPoses6D->size();
                ROS_INFO("There are in total %d keyframes",keyframeN);
                for (int i=0;i<keyframeN;i++){
                    pcl::PointCloud<PointType>::Ptr cornerKeyFrame(new pcl::PointCloud<PointType>());
                    pcl::PointCloud<PointType>::Ptr surfKeyFrame(new pcl::PointCloud<PointType>());
                    string cornerFileName = loadKeyframeMapDirectory + "/corner"+ to_string(i) + ".pcd";
                    string surfFileName = loadKeyframeMapDirectory + "/surf"+ to_string(i) + ".pcd";
                    if (pcl::io::loadPCDFile<PointType> (cornerFileName, *cornerKeyFrame) == -1) 
                       cout<< "Couldn't read file"+ cornerFileName <<endl;
                    if (pcl::io::loadPCDFile<PointType> (surfFileName, *surfKeyFrame) == -1) 
                       cout<< "Couldn't read file"+ surfFileName <<endl;
                    cornerCloudKeyFrames.push_back(cornerKeyFrame);
                    surfCloudKeyFrames.push_back(surfKeyFrame);
                    if (i%100 == 0)
                        cout << "\r" << std::flush << "Loading feature cloud " << i << " of " << keyframeN-1 << " ...\n";
                }
                ROS_INFO("************************Keyframe map loaded************************");
                mapLoaded=true;
            }
        }
        
    }

    void allocateMemory()
    {
        affine_lidar_to_map = Eigen::Affine3f::Identity();
        affine_lidar_to_odom = Eigen::Affine3f::Identity();
        affine_odom_to_map = Eigen::Affine3f::Identity();
        mergeCorrection = Eigen::Affine3f::Identity();

        edgeErrorCloud.reset(new pcl::PointCloud<PointType>()); 
        surfErrorCloud.reset(new pcl::PointCloud<PointType>()); 

        submap.reset(new pcl::PointCloud<PointType>()); // why dot when it is pointer type

        lidarCloudRaw.reset(new pcl::PointCloud<PointType>()); 

        cloudKeyPoses3D.reset(new pcl::PointCloud<PointType>());
        cloudKeyPoses6D.reset(new pcl::PointCloud<PointTypePose>());

        keyPosesTarget3D.reset(new pcl::PointCloud<PointType>());
        keyPosesTarget6D.reset(new pcl::PointCloud<PointTypePose>());

        copy_cloudKeyPoses3D.reset(new pcl::PointCloud<PointType>());
        copy_cloudKeyPoses6D.reset(new pcl::PointCloud<PointTypePose>());

        temporaryCloudKeyPoses3D.reset(new pcl::PointCloud<PointType>());
        temporaryCloudKeyPoses6D.reset(new pcl::PointCloud<PointTypePose>());

        kdtreeSurroundingKeyPoses.reset(new pcl::KdTreeFLANN<PointType>());
        kdtreeHistoryKeyPoses.reset(new pcl::KdTreeFLANN<PointType>());

        lidarCloudCornerLast.reset(new pcl::PointCloud<PointType>()); // corner feature set from odoOptimization
        lidarCloudSurfLast.reset(new pcl::PointCloud<PointType>()); // surf feature set from odoOptimization
        lidarCloudCornerLastDS.reset(new pcl::PointCloud<PointType>()); // downsampled corner featuer set from odoOptimization
        lidarCloudSurfLastDS.reset(new pcl::PointCloud<PointType>()); // downsampled surf featuer set from odoOptimization

        lidarCloudOri.reset(new pcl::PointCloud<PointType>());
        coeffSel.reset(new pcl::PointCloud<PointType>());

        lidarCloudOriCornerVec.resize(N_SCAN * Horizon_SCAN);
        coeffSelCornerVec.resize(N_SCAN * Horizon_SCAN);
        lidarCloudOriCornerFlag.resize(N_SCAN * Horizon_SCAN);
        lidarCloudOriSurfVec.resize(N_SCAN * Horizon_SCAN);
        coeffSelSurfVec.resize(N_SCAN * Horizon_SCAN);
        lidarCloudOriSurfFlag.resize(N_SCAN * Horizon_SCAN);

        std::fill(lidarCloudOriCornerFlag.begin(), lidarCloudOriCornerFlag.end(), false);
        std::fill(lidarCloudOriSurfFlag.begin(), lidarCloudOriSurfFlag.end(), false);

        lidarCloudCornerFromMap.reset(new pcl::PointCloud<PointType>());
        lidarCloudSurfFromMap.reset(new pcl::PointCloud<PointType>());
        lidarCloudCornerFromMapDS.reset(new pcl::PointCloud<PointType>());
        lidarCloudSurfFromMapDS.reset(new pcl::PointCloud<PointType>());

        kdtreeCornerFromMap.reset(new pcl::KdTreeFLANN<PointType>());
        kdtreeSurfFromMap.reset(new pcl::KdTreeFLANN<PointType>());

        for (int i = 0; i < 6; ++i){
            transformBeforeMapped[i] = 0;
            transformTobeMapped[i] = 0;
        }
        
        matP = cv::Mat(6, 6, CV_32F, cv::Scalar::all(0));
    }

    void odometryMsgToAffine3f(const nav_msgs::Odometry::ConstPtr& msgIn,Eigen::Affine3f &trans)
    {
        tf::Quaternion tfQ(msgIn->pose.pose.orientation.x,msgIn->pose.pose.orientation.y,msgIn->pose.pose.orientation.z,msgIn->pose.pose.orientation.w);
        double roll,pitch,yaw;
        tf::Matrix3x3(tfQ).getRPY(roll,pitch,yaw);
        // think about why not update affine_lidar_to_odom and affine_lidar_to_map here!!!
        trans = pcl::getTransformation(msgIn->pose.pose.position.x,
        msgIn->pose.pose.position.y,msgIn->pose.pose.position.z, float(roll),float(pitch),float(yaw));
    }
     void odometryMsgToTrans(const nav_msgs::Odometry::ConstPtr& msgIn,float trans[6])
    {
        tf::Quaternion tfQ(msgIn->pose.pose.orientation.x,msgIn->pose.pose.orientation.y,msgIn->pose.pose.orientation.z,msgIn->pose.pose.orientation.w);
        double roll,pitch,yaw;
        tf::Matrix3x3(tfQ).getRPY(roll,pitch,yaw);
        trans[0] = roll;
        trans[1] = pitch;
        trans[2] = yaw;
        trans[3] = msgIn->pose.pose.position.x;
        trans[4] = msgIn->pose.pose.position.y;
        trans[5] = msgIn->pose.pose.position.z;
    }

    void lidarCloudInfoHandler(const kloam::cloud_infoConstPtr& msgIn)
    {
        mtx.lock();
        cloudInfoBuffer.push(msgIn);
        mtx.unlock();
    }

    void lidarOdometryHandler(const nav_msgs::Odometry::ConstPtr& msgIn)
    {
        mtx.lock();
        lidarOdometryBuffer.push(msgIn);
        mtx.unlock();

        Eigen::Affine3f affine_lidar_to_odom_tmp;
        odometryMsgToAffine3f(msgIn, affine_lidar_to_odom_tmp); // why not okay when in function?
        float odomTmp[6];
        Affine3f2Trans(affine_lidar_to_odom_tmp, odomTmp);
        // cout<<"odom message: "<<odomTmp[3]<<" "<< odomTmp[4]<<endl;
        // high-frequency publish
        Eigen::Affine3f affine_lidar_to_map_tmp = affine_odom_to_map*affine_lidar_to_odom_tmp;
        float array_lidar_to_map[6];
        Affine3f2Trans(affine_lidar_to_map_tmp, array_lidar_to_map);
        tf::Quaternion q = tf::createQuaternionFromRPY(array_lidar_to_map[0],array_lidar_to_map[1],array_lidar_to_map[2]);
        // cout<<"map message: "<<array_lidar_to_map[3]<<" "<< array_lidar_to_map[4]<<endl;
        nav_msgs::Odometry odomAftMapped;
        odomAftMapped.header.frame_id = mapFrame;
        odomAftMapped.header.stamp = msgIn->header.stamp;
        odomAftMapped.pose.pose.orientation.x = q.x();
        odomAftMapped.pose.pose.orientation.y = q.y();
        odomAftMapped.pose.pose.orientation.z = q.z();
        odomAftMapped.pose.pose.orientation.w = q.w();
        odomAftMapped.pose.pose.position.x = array_lidar_to_map[3];
        odomAftMapped.pose.pose.position.y = array_lidar_to_map[4];
        odomAftMapped.pose.pose.position.z = array_lidar_to_map[5];
        pubLidarOdometryGlobalFusion.publish(odomAftMapped);

        // Publish TF
        static tf::TransformBroadcaster br;
        tf::Transform t_odom_to_lidar = tf::Transform(tf::createQuaternionFromRPY(array_lidar_to_map[0], array_lidar_to_map[1], array_lidar_to_map[2]),
                                                      tf::Vector3(array_lidar_to_map[3], array_lidar_to_map[4], array_lidar_to_map[5]));
        tf::StampedTransform trans_odom_to_lidar = tf::StampedTransform(t_odom_to_lidar, msgIn->header.stamp, mapFrame, lidarFrame);
        br.sendTransform(trans_odom_to_lidar);
        // // sometimes it cannot find tf when bagtime is not the same with the sensor time
        // tf::TransformListener tf_;
        // tf::StampedTransform stamped_lidar_to_baselink;
        // try
        // {
        //     tf_.lookupTransform(baselinkFrame, lidarFrame, msgIn->header.stamp, stamped_lidar_to_baselink);
        // }
        // catch(tf::TransformException e)
        // {
        //     ROS_ERROR("Failed to compute lidar_to_baselink: (%s)", e.what());
        //     return;
        // }
        // tf::Transform t_odom_to_baselink = t_odom_to_lidar*stamped_lidar_to_baselink;
        // child frame 'lidar_link' expressed in parent_frame 'mapFrame'
        // tf::StampedTransform trans_odom_to_baselink = tf::StampedTransform(t_odom_to_baselink, msgIn->header.stamp, mapFrame, baselinkFrame);
        // br.sendTransform(trans_odom_to_baselink);


        geometry_msgs::PoseStamped pose_stamped;
        pose_stamped.pose = odomAftMapped.pose.pose;
        pose_stamped.header.frame_id = mapFrame;
        pose_stamped.header.stamp = msgIn->header.stamp;
        globalPathFusion.poses.push_back(pose_stamped);
        globalPathFusion.header.stamp = msgIn->header.stamp;
        globalPathFusion.header.frame_id = mapFrame;
        pubPathFusion.publish(globalPathFusion); // before loop closure


        static bool init_flag = true;
        if (init_flag==true)
        {
            H_init = affine_lidar_to_map_tmp;
            init_flag=false;
        }
        Eigen::Affine3f H_rot;
        
        // for benchmarking in kitti
        // how kitti camera frame (xright y down z forward) rotates into REP103
        // kitti x uses -y from kloam
        // H_rot.matrix() << 0,-1,0,0, 
        //                     0,0,-1,0,
        //                     1,0,0,0,
        //                     0,0,0,1;

        // for fusion_pose output
        H_rot.matrix() << 1,0,0,0, 
                            0,1,0,0,
                            0,0,1,0,
                            0,0,0,1;
        Eigen::Affine3f H = affine_lidar_to_map_tmp;
        H = H_rot*H_init.inverse()*H; //to get H12 = H10*H02 , 180 rot according to z axis
        pose_kitti_vec.push_back(H);
    }

    void transformUpdate()
    {
        mtx.lock();
        affine_lidar_to_map =  trans2Affine3f(transformTobeMapped);
        affine_odom_to_map = affine_lidar_to_map*affine_lidar_to_odom.inverse();
        // cout<<"affine_lidar_to_map "<<affine_lidar_to_map.matrix()<<endl;
        // cout<<"affine_odom_to_map "<<affine_odom_to_map.matrix()<<endl;
        // // Publish TF
        // static tf::TransformBroadcaster br;
        // tf::Transform t_odom_to_lidar = tf::Transform(tf::createQuaternionFromRPY(transformTobeMapped[0], transformTobeMapped[1], transformTobeMapped[2]),
        //                                               tf::Vector3(transformTobeMapped[3], transformTobeMapped[4], transformTobeMapped[5]));
        // // child frame 'lidar_link' expressed in parent_frame 'mapFrame'
        // tf::StampedTransform trans_odom_to_lidar = tf::StampedTransform(t_odom_to_lidar, timeLidarInfoStamp, mapFrame, lidarFrame);
        // br.sendTransform(trans_odom_to_lidar);

        mtx.unlock();
    }
    void run()
    {
        while(ros::ok()){ // why while(1) is not okay???
            while (!cloudInfoBuffer.empty() && !lidarOdometryBuffer.empty())
            {
                mtx.lock();
                while (!lidarOdometryBuffer.empty() && lidarOdometryBuffer.front()->header.stamp.toSec() < cloudInfoBuffer.front()->header.stamp.toSec())
                {
                    lidarOdometryBuffer.pop();
                }
                if (lidarOdometryBuffer.empty()){
                    mtx.unlock();
                    break;
                }
                timeLidarInfoStamp = cloudInfoBuffer.front()->header.stamp;
                cloudInfoTime = cloudInfoBuffer.front()->header.stamp.toSec();
                double lidarOdometryTime = lidarOdometryBuffer.front()->header.stamp.toSec();
                if (rosTimeStart < 0) rosTimeStart = lidarOdometryTime;

                if (lidarOdometryTime != cloudInfoTime) // normally >, so pop one cloud_info msg
                {
                    ROS_WARN("Unsync message!");
                    cloudInfoBuffer.pop();  // pop the old one,otherwise it  will go to dead loop, different from aloam 
                    mtx.unlock();
                    break;
                }

                // extract info and feature cloud
                kloam::cloud_infoConstPtr cloudInfoMsg = cloudInfoBuffer.front();
                nav_msgs::Odometry::ConstPtr lidarOdometryMsg =  lidarOdometryBuffer.front();

                lidarCloudRaw.reset(new pcl::PointCloud<PointType>()); 
                cloudInfo = *cloudInfoMsg;
                odometryMsgToAffine3f(lidarOdometryMsg,affine_lidar_to_odom);
                odometryError = lidarOdometryMsg->twist.twist.linear.x; // twist.covariance

                pcl::fromROSMsg(cloudInfoMsg->cloud_corner,  *lidarCloudCornerLast);
                pcl::fromROSMsg(cloudInfoMsg->cloud_surface, *lidarCloudSurfLast);
                pcl::fromROSMsg(cloudInfoMsg->cloud_raw,  *lidarCloudRaw);
                // clear
                lidarOdometryBuffer.pop(); 
                while (!cloudInfoBuffer.empty())
                {
                    cloudInfoBuffer.pop();
                    // ROS_INFO_STREAM("popping old cloud_info messages for real-time performance");
                }
                mtx.unlock();

                TicToc mapping;
                updateInitialGuess(); // actually the same as ALOAM

                if (localizationMode && !relocSuccess) break;

                TicToc extract;
                extractNearby();
                if (frameTobeAbandoned == true) 
                {
                    cout<<"frame of tmm entry is abandoned after cloud extraction"<<endl;
                    frameTobeAbandoned = false;
                    transformUpdate(); // need it for initial guess in TMM
                    break;
                }
                // cout<<"extract: "<<extract.toc()<<endl;
                TicToc downsample;
                downsampleCurrentScan();
                // cout<<"downsample: "<<downsample.toc()<<endl;
                TicToc opt;
                scan2MapOptimization();

                if (frameTobeAbandoned == true) 
                {
                    cout<<"frame of tmm entry is abandoned after opt"<<endl;
                    frameTobeAbandoned = false;
                    transformUpdate(); // need it for initial guess in TMM
                    break;
                }

                float optTime = opt.toc();
                // cout<<"optimization: "<<optTime<<endl; // > 90% of the total time
                
                TicToc optPose;
                if (localizationMode)
                {
                    saveTemporaryKeyframes();
                    updatePathRELOC(cloudInfoMsg);  // for visualizing in rviz
                }
                else
                {
                    saveKeyFramesAndFactor();
                    correctPoses();
                }
                // float optPoseTime = optPose.toc();
                // cout<<"pose opt. takes "<< optPoseTime<<endl;
                // TicToc publish;
                publishFrames();
                publishOdometry();
                transformUpdate();
                poorlyMatched = false;
                // cout<<"publish: "<<publish.toc()<<endl;
                // printTrans("after mapping: ",transformTobeMapped);
                mappingTimeVec.push_back(mapping.toc());
                if (mappingTimeVec.back() > 3000)
                {
                    ROS_WARN_STREAM("Mapping: "<< mappingTimeVec.back()  <<" ms; Opt. time: "<<optTime<<"ms; "
                        <<"Iterations: "<<iterCount<<"; edge corr: "<<edgePointCorrNum<<"; surf corr: "<<surfPointCorrNum);
                    // pcl::io::savePCDFileBinary(saveMapDirectory + "/mapping_too_long"+ std::to_string(cloudInfoTime)+".pcd", *lidarCloudRaw);
                }
                if (goodToMergeMap)
                {
                    if (mapUpdateEnabled)
                        mergeMap();
                    // update the initial guess for the next scan!!!
                    affine_odom_to_map = mergeCorrection*affine_odom_to_map;
                    // downsize temporary maps to slidingWindowSize
                    auto iteratorKeyPoses3D = temporaryCloudKeyPoses3D->begin();
                    auto iteratorKeyPoses6D = temporaryCloudKeyPoses6D->begin();
                    auto iteratorKeyFramesC = temporaryCornerCloudKeyFrames.begin();
                    auto iteratorKeyFramesS = temporarySurfCloudKeyFrames.begin();
                    // usually added cloud would not be big so just leave the sparsification to savingMap
                    ROS_INFO_STREAM("At time "<< cloudInfoTime - rosTimeStart<< " sec, Merged map has "<<(int)temporaryCloudKeyPoses3D->size()<< " key poses");
                    while ((int)temporaryCloudKeyPoses3D->size() > slidingWindowSize)
                    {
                        temporaryCloudKeyPoses3D->erase(iteratorKeyPoses3D);
                        temporaryCloudKeyPoses6D->erase(iteratorKeyPoses6D);
                        temporaryCornerCloudKeyFrames.erase(iteratorKeyFramesC);
                        temporarySurfCloudKeyFrames.erase(iteratorKeyFramesS);
                    }
                    // cout<<temporaryCloudKeyPoses3D->size()<<endl;
                    // cout<<"reindexing: key poses and key frames are corresponding with respect to the adding sequence"<<endl;
                    for (int i = 0 ; i< (int)temporaryCloudKeyPoses3D->size(); i++)
                    {
                        temporaryCloudKeyPoses3D->points[i].intensity = i;
                        temporaryCloudKeyPoses6D->points[i].intensity = i;
                    }
                    goodToMergeMap = false;
                    temporaryMappingMode = false;

                }
                
            }


        }

    }

    void mergeMap()
    {
        cout<<" DO gtsam optimization here"<<endl;
        TicToc t_merge;
        int backTracing = 10;
        int priorNode = startTemporaryMappingIndex - backTracing;
        if (priorNode < 0 ) priorNode = 0;

        // gtsam
        NonlinearFactorGraph gtSAMgraphTM;
        Values initialEstimateTM;
        Values optimizedEstimateTM;
        ISAM2 *isamTM;
        Values isamCurrentEstimateTM;
        ISAM2Params parameters;
        parameters.relinearizeThreshold = 0.1;
        parameters.relinearizeSkip = 1;
        isamTM = new ISAM2(parameters);
        noiseModel::Diagonal::shared_ptr priorNoise = noiseModel::Diagonal::Variances((Vector(6) << 1e-2, 1e-2, 1e-2, 1e-1, 1e-1, 1e-1).finished()); // rad*rad, meter*meter
        gtsam::Pose3 posePrior = pclPointTogtsamPose3(temporaryCloudKeyPoses6D->points[priorNode]);
        gtSAMgraphTM.add(PriorFactor<Pose3>(priorNode, posePrior, priorNoise));
        initialEstimateTM.insert(priorNode, posePrior);

        int tempSize = temporaryCloudKeyPoses6D->points.size();
        if (tempSize < 3 ) return;
        for (int i = priorNode; i < tempSize - 2; i++)
        {
            noiseModel::Diagonal::shared_ptr odometryNoise = noiseModel::Diagonal::Variances((Vector(6) <<1e-4, 1e-4, 1e-4, 1e-3, 1e-3, 1e-3 ).finished());
            gtsam::Pose3 poseFrom = pclPointTogtsamPose3(temporaryCloudKeyPoses6D->points[i]);
            gtsam::Pose3 poseTo   = pclPointTogtsamPose3(temporaryCloudKeyPoses6D->points[i+1]);
            gtSAMgraphTM.add(BetweenFactor<Pose3>(i,i+1, poseFrom.between(poseTo), odometryNoise));
            initialEstimateTM.insert(i+1, poseTo);
            // update iSAM
            isamTM->update(gtSAMgraphTM, initialEstimateTM);
            isamTM->update();
            gtSAMgraphTM.resize(0);
            initialEstimateTM.clear();
        }

        Eigen::Affine3f wrongPose = pclPointToAffine3f(temporaryCloudKeyPoses6D->points[tempSize -1 ]);
        Eigen::Affine3f correctedPose = mergeCorrection * wrongPose;
        gtsam::Pose3 poseCorr = Affine3f2gtsamPose(correctedPose);

        // cout<<" add prior factor instead to constrain the covariances"<<endl;
        // cannot put it in the loop above
        noiseModel::Diagonal::shared_ptr odometryNoise = noiseModel::Diagonal::Variances((Vector(6) << 1e-6, 1e-6, 1e-6, 1e-4, 1e-4, 1e-4).finished());
        gtsam::Pose3 poseFrom = pclPointTogtsamPose3(temporaryCloudKeyPoses6D->points[tempSize - 2 ]);
        gtsam::Pose3 poseTo = pclPointTogtsamPose3(temporaryCloudKeyPoses6D->points[tempSize - 1 ]);
        gtSAMgraphTM.add(BetweenFactor<Pose3>(tempSize - 2 , tempSize -1, poseFrom.between(poseTo), odometryNoise));

        // adding two priorFactors will make the opt. go wild
        noiseModel::Diagonal::shared_ptr corrNoise = noiseModel::Diagonal::Variances((Vector(6) << 1e-4, 1e-4, 1e-4, 1e-3, 1e-3, 1e-3).finished()); // rad*rad, meter*meter
        gtSAMgraphTM.add(PriorFactor<Pose3>(tempSize - 1, poseCorr, corrNoise));
        initialEstimateTM.insert(tempSize - 1, poseCorr);
        
        // cout<<"before opt. "<< poseCorr.translation().x()<<" "<< poseCorr.translation().y()<<" "<< poseCorr.translation().z()<<endl;

        // update iSAM
        isamTM->update(gtSAMgraphTM, initialEstimateTM);
        isamTM->update();
        isamTM->update();
        isamTM->update();
        gtSAMgraphTM.resize(0);
        initialEstimateTM.clear();

        cout<<"pose correction"<<endl;
        isamCurrentEstimateTM = isamTM->calculateEstimate();

        std::vector<int> keyPoseSearchIdx;
        std::vector<float> keyPoseSearchDist;
        for (int i = priorNode; i < tempSize; i++)
        {
            // change every loop
            pcl::KdTreeFLANN<PointType>::Ptr keyPosesTree(new pcl::KdTreeFLANN<PointType>());
            keyPosesTree->setInputCloud(cloudKeyPoses3D);
            auto poseCov = isamTM->marginalCovariance(i);
            // cout<<  "x cov: " <<poseCov(3,3)  << " y cov: "<<poseCov(4,4)<<endl;

            temporaryCloudKeyPoses3D->points[i].x = isamCurrentEstimateTM.at<Pose3>(i).translation().x();
            temporaryCloudKeyPoses3D->points[i].y = isamCurrentEstimateTM.at<Pose3>(i).translation().y();
            temporaryCloudKeyPoses3D->points[i].z = isamCurrentEstimateTM.at<Pose3>(i).translation().z();

            temporaryCloudKeyPoses6D->points[i].x = temporaryCloudKeyPoses3D->points[i].x;
            temporaryCloudKeyPoses6D->points[i].y = temporaryCloudKeyPoses3D->points[i].y;
            temporaryCloudKeyPoses6D->points[i].z = temporaryCloudKeyPoses3D->points[i].z;
            temporaryCloudKeyPoses6D->points[i].roll  = isamCurrentEstimateTM.at<Pose3>(i).rotation().roll();
            temporaryCloudKeyPoses6D->points[i].pitch = isamCurrentEstimateTM.at<Pose3>(i).rotation().pitch();
            temporaryCloudKeyPoses6D->points[i].yaw   = isamCurrentEstimateTM.at<Pose3>(i).rotation().yaw();

            // temporaryCloudKeyPoses3D->points[i].intensity = i; // no change here actually
            // temporaryCloudKeyPoses6D->points[i].intensity = i;

            keyPosesTree->nearestKSearch(temporaryCloudKeyPoses3D->points[i],1, keyPoseSearchIdx, keyPoseSearchDist);

            mtx.lock();
            if (keyPoseSearchDist[0] < 2*surroundingKeyframeDensity)
            {
                // cout<<keyPoseSearchIdx[0]<<endl;
                cloudKeyPoses3D->erase(cloudKeyPoses3D->begin() + keyPoseSearchIdx[0]);
                cloudKeyPoses6D->erase(cloudKeyPoses6D->begin() + keyPoseSearchIdx[0]);
                cornerCloudKeyFrames.erase(cornerCloudKeyFrames.begin() + keyPoseSearchIdx[0]);
                surfCloudKeyFrames.erase(surfCloudKeyFrames.begin() + keyPoseSearchIdx[0]);
            }
            mtx.unlock();
        }
        for (int i = priorNode; i < tempSize; i++)
        {
            cloudKeyPoses3D->push_back(temporaryCloudKeyPoses3D->points[i]); // no "points." in between!!!
            cloudKeyPoses6D->push_back(temporaryCloudKeyPoses6D->points[i]);
            cornerCloudKeyFrames.push_back(temporaryCornerCloudKeyFrames[i]);
            surfCloudKeyFrames.push_back(temporarySurfCloudKeyFrames[i]); 
        }
        cout<<"map merge takes "<<t_merge.toc()<< " ms"<<endl; // negligible

        // reindexing
        for (int i = 0; i < (int) cloudKeyPoses3D->size(); i++)
        {
            cloudKeyPoses3D->points[i].intensity = i;
            cloudKeyPoses6D->points[i].intensity = i;
        }

    }

    void updatePathRELOC(const kloam::cloud_infoConstPtr& msgIn){
        geometry_msgs::PoseStamped pose_stamped;
        pose_stamped.header.stamp = msgIn->header.stamp;
        pose_stamped.header.frame_id = mapFrame;
        pose_stamped.pose.position.x = transformTobeMapped[3];
        pose_stamped.pose.position.y = transformTobeMapped[4];
        pose_stamped.pose.position.z = transformTobeMapped[5];
        tf::Quaternion q = tf::createQuaternionFromRPY(transformTobeMapped[0],transformTobeMapped[1],transformTobeMapped[2]);
        pose_stamped.pose.orientation.x = q.x();
        pose_stamped.pose.orientation.y = q.y();
        pose_stamped.pose.orientation.z = q.z();
        pose_stamped.pose.orientation.w = q.w();
        globalPath.poses.push_back(pose_stamped);
    }

    void gpsHandler(const sensor_msgs::NavSatFixConstPtr& gpsMsg)
    {
        if (gpsMsg->status.status != 0) 
        {
            cout<<gpsMsg->status.status<<endl;
            ROS_WARN("Bad gps message!");
            return;
        }
        // string gpsFrame = "/gps";
        // tf::TransformListener listener;
        // tf::StampedTransform transformStamped_lidar_to_gps;
        // try
        // {
        //     // ERROR] [1639030193.661625830, 1521156822.162099906]: "lidar_link" passed to lookupTransform argument target_frame does not exist
        //     listener.waitForTransform(lidarFrame, mapFrame, gpsMsg->header.stamp, ros::Duration(0.1));
        //     listener.lookupTransform(lidarFrame, mapFrame, gpsMsg->header.stamp, transformStamped_lidar_to_gps);
        // }
        // catch (tf::TransformException &ex) 
        // {
        //     ROS_ERROR("%s",ex.what());
        //     return;
        // }
        // okay to do static transform
        // tf::Transform transform_lidar_to_gps = transformStamped_lidar_to_gps;
        // nclt
        tf::Transform transform_lidar_to_gps(tf::createQuaternionFromRPY(0.014,0.019,0),tf::Vector3(-0.002,0.024,0.95669));
        // usyd[0, -0.122, 0]"
        // tf::Transform transform_lidar_to_gps(tf::createQuaternionFromRPY(0.045,0.358,-0.913),tf::Vector3(0,-0.122,0));
        double x,y,z;
        x = sin(deg2rad(gpsMsg->latitude - lati0))*rns;
        y = sin(deg2rad(gpsMsg->longitude - longi0))*rew*cos(deg2rad(lati0));
        z = alti0 - gpsMsg->altitude;
        tf::Transform transform_gps_to_gps0(tf::Quaternion(0, 0, 0, 1), tf::Vector3(x,y,z));
        // transform_lidar_to_gps = transform_lidar0_to_gps0
        tf::Transform transform_lidar_to_lidar0 = transform_lidar_to_gps*transform_gps_to_gps0*transform_lidar_to_gps.inverse();
        // cout<<"gps x y z: "<<x<<" "<<y<<" "<<z<<endl;
        tf::Vector3 origin = transform_lidar_to_lidar0.getOrigin();
        // cout<<"after conversion x y z: "<<origin.x()<<" "<<origin.y()<<" "<<origin.z()<<endl;

        nav_msgs::Odometry odomGPS;
        // notice PoseWithCovariance order: # (x, y, z, rotation about X axis, rotation about Y axis, rotation about Z axis; float64[36] covariance
        // different from GTSAM pose covariance order
        odomGPS.pose.covariance[0] = gpsMsg->position_covariance[0];
        odomGPS.pose.covariance[7] = gpsMsg->position_covariance[4];
        odomGPS.pose.covariance[14] = gpsMsg->position_covariance[8];
        odomGPS.pose.pose.position.x = origin.x();
        odomGPS.pose.pose.position.y = origin.y();
        odomGPS.pose.pose.position.z = origin.z();
        odomGPS.header = gpsMsg->header;
        gpsQueue.push_back(odomGPS);
        gpsVec.push_back(odomGPS);

    }
     void gtHandler(const nav_msgs::Odometry::ConstPtr& gtMsg)
    {
        if (!relocSuccess)       gtVec.push_back(gtMsg);
    }

    void pointAssociateToMap(PointType const * const pi, PointType * const po)
    {
        po->x = affine_lidar_to_map(0,0) * pi->x + affine_lidar_to_map(0,1) * pi->y + affine_lidar_to_map(0,2) * pi->z + affine_lidar_to_map(0,3);
        po->y = affine_lidar_to_map(1,0) * pi->x + affine_lidar_to_map(1,1) * pi->y + affine_lidar_to_map(1,2) * pi->z + affine_lidar_to_map(1,3);
        po->z = affine_lidar_to_map(2,0) * pi->x + affine_lidar_to_map(2,1) * pi->y + affine_lidar_to_map(2,2) * pi->z + affine_lidar_to_map(2,3);
        po->intensity = pi->intensity;
    }

    pcl::PointCloud<PointType>::Ptr transformPointCloud(pcl::PointCloud<PointType>::Ptr cloudIn, PointTypePose* transformIn)
    {
        pcl::PointCloud<PointType>::Ptr cloudOut(new pcl::PointCloud<PointType>());

        int cloudSize = cloudIn->size();
        cloudOut->resize(cloudSize);

        Eigen::Affine3f transCur = pcl::getTransformation(transformIn->x, transformIn->y, transformIn->z, transformIn->roll, transformIn->pitch, transformIn->yaw);
        
        // #pragma omp parallel for num_threads(numberOfCores)
        for (int i = 0; i < cloudSize; ++i)
        {
            const auto &pointFrom = cloudIn->points[i];
            cloudOut->points[i].x = transCur(0,0) * pointFrom.x + transCur(0,1) * pointFrom.y + transCur(0,2) * pointFrom.z + transCur(0,3);
            cloudOut->points[i].y = transCur(1,0) * pointFrom.x + transCur(1,1) * pointFrom.y + transCur(1,2) * pointFrom.z + transCur(1,3);
            cloudOut->points[i].z = transCur(2,0) * pointFrom.x + transCur(2,1) * pointFrom.y + transCur(2,2) * pointFrom.z + transCur(2,3);
            cloudOut->points[i].intensity = pointFrom.intensity;
        }
        return cloudOut;
    }

    gtsam::Pose3 pclPointTogtsamPose3(PointTypePose thisPoint)
    {
        return gtsam::Pose3(gtsam::Rot3::RzRyRx(double(thisPoint.roll), double(thisPoint.pitch), double(thisPoint.yaw)),
                                  gtsam::Point3(double(thisPoint.x),    double(thisPoint.y),     double(thisPoint.z)));
    }

    gtsam::Pose3 trans2gtsamPose(float transformIn[])
    {
        return gtsam::Pose3(gtsam::Rot3::RzRyRx(transformIn[0], transformIn[1], transformIn[2]), 
                                  gtsam::Point3(transformIn[3], transformIn[4], transformIn[5]));
    }

    Eigen::Affine3f pclPointToAffine3f(PointTypePose thisPoint)
    { 
        return pcl::getTransformation(thisPoint.x, thisPoint.y, thisPoint.z, thisPoint.roll, thisPoint.pitch, thisPoint.yaw);
    }

    gtsam::Pose3 Affine3f2gtsamPose(Eigen::Affine3f aff){
        float transformOut[6] = {0};
        Affine3f2Trans(aff, transformOut);
        return gtsam::Pose3(gtsam::Rot3::RzRyRx(transformOut[0], transformOut[1], transformOut[2]), 
                                  gtsam::Point3(transformOut[3], transformOut[4], transformOut[5]));
    }
    PointTypePose trans2PointTypePose(float transformIn[])
    {
        PointTypePose thisPose6D;
        thisPose6D.x = transformIn[3];
        thisPose6D.y = transformIn[4];
        thisPose6D.z = transformIn[5];
        thisPose6D.roll  = transformIn[0];
        thisPose6D.pitch = transformIn[1];
        thisPose6D.yaw   = transformIn[2];
        return thisPose6D;
    }

    
    bool saveMapService(kloam::save_mapRequest& req, kloam::save_mapResponse& res)
    {

        float mappingTime = accumulate(mappingTimeVec.begin(),mappingTimeVec.end(),0.0);
        cout<<"Average time consumed by mapping is :"<<mappingTime/mappingTimeVec.size()<<" ms"<<endl;
        if (localizationMode) cout<<"Times of entering TMM is :"<<TMMcount<<endl;

        if (saveMatchingError)
        {
        // saving odometry error
            string fileName = saveMapDirectory + "/mappingError.txt";
            ofstream mapErrorFile(fileName);
            mapErrorFile.setf(ios::fixed, ios::floatfield);  // 设定为 fixed 模式，以小数点表示浮点数
            mapErrorFile.precision(6); // 固定小数位6
            if(!mapErrorFile.is_open())
            {
                cout<<"Cannot open "<<fileName<<endl;
                return false;
            }
            for (int i=0; i<(int)mappingLogs.size();i++)
            {
                vector<double> tmp = mappingLogs[i];
                mapErrorFile<<" "<<tmp[0]<<" "<<tmp[1]<<" "<<tmp[2]<<" "<<tmp[3]<<" "<<tmp[4]<<" " <<tmp[5]<<" " <<tmp[6]<<" " <<tmp[7]<<" " <<tmp[8]<<"\n";
            }
            mapErrorFile.close();
            cout<<"Done saving mapping error file!"<<endl;
        }
  // saving pose estimates and GPS signals
        if (savePose)
        {
            // ofstream pose_file;
            cout<<"Recording trajectory..."<<endl;
            // string fileName = saveMapDirectory+"/pose_fusion_kitti.txt";
            // pose_file.open(fileName,ios::out);
            // pose_file.setf(std::ios::scientific, std::ios::floatfield);
            // pose_file.precision(6);
            // if(!pose_file.is_open())
            // {
            //     cout<<"Cannot open "<<fileName<<endl;
            //     return false;
            // }
            // // keyposes: kitti form (z forward x right y downward)
            // cout<<"Number of poses: "<<pose_kitti_vec.size()<<endl;
            // for (int i = 0; i <(int)pose_kitti_vec.size(); ++i)
            // {
            //     Eigen::Affine3f pose_kitti = pose_kitti_vec[i];
            //     pose_file<<pose_kitti(0,0)<<" "<<pose_kitti(0,1)<<" "<<pose_kitti(0,2)<<" "<<pose_kitti(0,3)<<" "
            //         <<pose_kitti(1,0)<<" "<<pose_kitti(1,1)<<" "<<pose_kitti(1,2)<<" "<<pose_kitti(1,3)<<" "
            //         <<pose_kitti(2,0)<<" "<<pose_kitti(2,1)<<" "<<pose_kitti(2,2)<<" "<<pose_kitti(2,3)<<"\n";

            // }
            // pose_file.close();


            // // 2nd: keyposes
            // int pointN = (int)globalPath.poses.size();
            // cout<< "There are "<<pointN<<" keyframes in total"<<endl;
            // for (int i = 0; i < pointN; ++i)
            // {
            //     geometry_msgs::PoseStamped tmp = globalPath.poses[i];
            //     pose_file<<tmp.pose.position.x<<" "<<tmp.pose.position.y<<" "<<tmp.pose.position.z<<"\n";
            // }
            // // 3rd: odometry msgs
            // int pointN = (int)globalOdometry.size();
            // cout<< "There are "<<pointN<<" in total"<<endl;
            // for (int i = 0; i < pointN; ++i)
            // {
            //     nav_msgs::Odometry tmp = globalOdometry[i];
            //     pose_file<<tmp.pose.pose.position.x<<" "<<tmp.pose.pose.position.y<<"\n";
            // }
            // pose_file.close();

            // 4th: stamped pose for odometry gt
            ofstream pose_file2;
            string fileName2 = saveMapDirectory+"/path_mapping.txt";
            pose_file2.open(fileName2,ios::out);
            pose_file2.setf(ios::fixed, ios::floatfield);  // 设定为 fixed 模式，以小数点表示浮点数
            pose_file2.precision(6); // 固定小数位6
            int pointN = (int)globalOdometry.size();
            cout<<"mapping pose size: "<<pointN<<endl;
            for (int i = 0; i < pointN; ++i)
            {
                nav_msgs::Odometry tmp = globalOdometry[i];
                double r,p,y;
                tf::Quaternion q(tmp.pose.pose.orientation.x,tmp.pose.pose.orientation.y, tmp.pose.pose.orientation.z,tmp.pose.pose.orientation.w);
                tf::Matrix3x3(q).getRPY(r,p,y);
                // save it in nano sec to compare it with the nclt gt
                pose_file2<<tmp.header.stamp.toSec()*1e+6<<" "<<tmp.pose.pose.position.x<<" "<<tmp.pose.pose.position.y<<" "<<
                tmp.pose.pose.position.z<<" "<<r<<" "<<p<<" "<<y<<" "<<"\n";
            }
            pose_file2.close();
            // higher frequency odometry
            ofstream pose_file3;
            string fileName3 = saveMapDirectory+"/path_fusion.txt";
            pose_file3.open(fileName3,ios::out);
            pose_file3.setf(ios::fixed, ios::floatfield);  // 设定为 fixed 模式，以小数点表示浮点数
            pose_file3.precision(6); // 固定小数位6
            int pointN2 = (int)globalPathFusion.poses.size();
            cout<<"fusion pose size: "<<pointN2<<endl;
            for (int i = 0; i < pointN2; ++i)
            {
                geometry_msgs::PoseStamped tmp = globalPathFusion.poses[i];
                double r,p,y;
                tf::Quaternion q(tmp.pose.orientation.x,tmp.pose.orientation.y, tmp.pose.orientation.z,tmp.pose.orientation.w);
                tf::Matrix3x3(q).getRPY(r,p,y);
                // save it in nano sec to compare it with the nclt gt
                pose_file3<<tmp.header.stamp.toSec()*1e+6<<" "<<tmp.pose.position.x<<" "<<tmp.pose.position.y<<" "<<
                tmp.pose.position.z<<" "<<r<<" "<<p<<" "<<y<<" "<<"\n";
            }
            pose_file3.close();

            cout<<"Trajectory recording finished!"<<endl;
            if (gpsVec.empty() == false){
                ofstream gps_file;
                gps_file.open(saveMapDirectory+"/gps.txt",ios::out);
                if(!gps_file.is_open())
                {
                    cout<<"Cannot open"<<saveMapDirectory+"/gps.txt"<<endl;
                }
                for (int i = 0; i <int(gpsVec.size()); ++i)
                {
                    nav_msgs::Odometry tmp = gpsVec[i];
                    gps_file<<tmp.pose.pose.position.x<<" "<<tmp.pose.pose.position.y<<" "<<tmp.pose.pose.position.z<<
                     " "<<tmp.pose.covariance[0]<<" "<<tmp.pose.covariance[7]<<" "<<tmp.pose.covariance[14]<<"\n";
                }
                gps_file.close();
                cout<< "GPS data size: "<<gpsVec.size()<<endl;
            }
        }


        // save keyframe map: for every keyframe, save keyframe pose, edge point pcd, surface point pcd
        // keyframe pose in one file
        // every keyframe has two other files: cornerI.pcd surfI.pcd
        if (savePCD)
        {
            cout << "****************************************************" << endl;
            cout << "Saving map to pcd files ..." << endl;

            cout << "Save destination: " << saveMapDirectory << endl;

            // save key frame transformations
            pcl::io::savePCDFileBinary(saveMapDirectory + "/trajectory.pcd", *cloudKeyPoses3D);
            pcl::io::savePCDFileBinary(saveMapDirectory + "/transformations.pcd", *cloudKeyPoses6D);
            // extract global point cloud map
            pcl::PointCloud<PointType>::Ptr globalCornerCloud(new pcl::PointCloud<PointType>());
            pcl::PointCloud<PointType>::Ptr globalCornerCloudDS(new pcl::PointCloud<PointType>());
            pcl::PointCloud<PointType>::Ptr globalSurfCloud(new pcl::PointCloud<PointType>());
            pcl::PointCloud<PointType>::Ptr globalSurfCloudDS(new pcl::PointCloud<PointType>());
            pcl::PointCloud<PointType>::Ptr globalMapCloud(new pcl::PointCloud<PointType>());
            for (int i = 0; i < (int)cloudKeyPoses3D->size(); i++) {
                *globalCornerCloud += *transformPointCloud(cornerCloudKeyFrames[i],  &cloudKeyPoses6D->points[i]);
                *globalSurfCloud   += *transformPointCloud(surfCloudKeyFrames[i],    &cloudKeyPoses6D->points[i]);
                // cout << "\r" << std::flush << "Processing feature cloud " << i << " of " << cloudKeyPoses6D->size() << " ...\n";
            }
            if(req.resolutionMap != 0)
            {
                cout << "\n\nSave resolution: " << req.resolutionMap << endl;

                // down-sample and save corner cloud
                downSizeFilterCorner.setInputCloud(globalCornerCloud);
                downSizeFilterCorner.setLeafSize(req.resolutionMap, req.resolutionMap, req.resolutionMap);
                downSizeFilterCorner.filter(*globalCornerCloudDS);
                pcl::io::savePCDFileBinary(saveMapDirectory + "/CornerMap.pcd", *globalCornerCloudDS);
                // down-sample and save surf cloud
                downSizeFilterSurf.setInputCloud(globalSurfCloud);
                downSizeFilterSurf.setLeafSize(req.resolutionMap, req.resolutionMap, req.resolutionMap);
                downSizeFilterSurf.filter(*globalSurfCloudDS);
                pcl::io::savePCDFileBinary(saveMapDirectory + "/SurfMap.pcd", *globalSurfCloudDS);
                // save error cloud
                // down-sample and save corner cloud
                downSizeFilterCorner.setInputCloud(edgeErrorCloud);
                downSizeFilterCorner.setLeafSize(req.resolutionMap, req.resolutionMap, req.resolutionMap);
                downSizeFilterCorner.filter(*edgeErrorCloud);
                downSizeFilterCorner.setInputCloud(surfErrorCloud);
                downSizeFilterCorner.setLeafSize(req.resolutionMap, req.resolutionMap, req.resolutionMap);
                downSizeFilterCorner.filter(*surfErrorCloud);
            }
            else
            {
                cout<<"No downsampling"<<endl;
                // save corner cloud
                pcl::io::savePCDFileBinary(saveMapDirectory + "/CornerMap.pcd", *globalCornerCloud);
                // save surf cloud
                pcl::io::savePCDFileBinary(saveMapDirectory + "/SurfMap.pcd", *globalSurfCloud);
            }
            pcl::PointCloud<PointType>::Ptr allErrorCloud(new pcl::PointCloud<PointType>());
            *allErrorCloud += *edgeErrorCloud;
            *allErrorCloud += *surfErrorCloud;

            // better compare edge and surf features
            for (int i = 0; i< (int)edgeErrorCloud->size(); i++){
                edgeErrorCloud->points[i].intensity = edgeErrorCloud->points[i].intensity/maxIntensity*255.0;
            }

            for (int i = 0; i< (int)surfErrorCloud->size(); i++){
                surfErrorCloud->points[i].intensity = surfErrorCloud->points[i].intensity/maxIntensity*255.0;
            }

            for (int i = 0; i< (int)edgeErrorCloud->size(); i++){
                allErrorCloud->points[i].intensity = allErrorCloud->points[i].intensity/maxIntensity*255.0;
            }

            pcl::io::savePCDFileBinary(saveMapDirectory + "/allErrorCloud.pcd", *allErrorCloud);


            pcl::io::savePCDFileBinary(saveMapDirectory + "/surfErrorCloud.pcd", *surfErrorCloud);
            pcl::io::savePCDFileBinary(saveMapDirectory + "/edgeErrorCloud.pcd", *edgeErrorCloud);
            // save global point cloud map
            *globalMapCloud += *globalCornerCloud;
            *globalMapCloud += *globalSurfCloud;

            int ret = pcl::io::savePCDFileBinary(saveMapDirectory + "/GlobalMap.pcd", *globalMapCloud);
            res.success = ret == 0;

            cout << "Saving map to pcd files completed\n" << endl;
        }
        
        if(saveKeyframeMap){ 
            int keyframeN = (int)cloudKeyPoses6D->size();

            cout<<"Map Saving to "+saveKeyframeMapDirectory<<endl;
            cout<<"There are "<<keyframeN<<" keyframes before downsampling"<<endl;

            cout<<"********************Saving keyframes and poses one by one**************************"<<endl;
            pcl::PointCloud<PointType>::Ptr cloudKeyPoses3DDS(new pcl::PointCloud<PointType>());
            if(req.resolutionKeyframe != 0)            downSizeFilterSavingKeyframes.setLeafSize(req.resolutionKeyframe, req.resolutionKeyframe, req.resolutionKeyframe);
            else         downSizeFilterSavingKeyframes.setLeafSize(2.0,2.0,2.0);
            downSizeFilterSavingKeyframes.setInputCloud(cloudKeyPoses3D); 
            downSizeFilterSavingKeyframes.filter(*cloudKeyPoses3DDS);
            int keyframeNDS = cloudKeyPoses3DDS->size();
            cout<<"There are "<<keyframeNDS<<" keyframes after downsampling"<<endl;
            ofstream pose_file;
            pose_file.open(saveKeyframeMapDirectory+"/poses.txt",ios::out); // downsampled
            if(!pose_file.is_open())
            {
                std::cout<<"Cannot open"<<saveKeyframeMapDirectory+"/poses.txt"<<std::endl;
                return false;
            }
            std::vector<int> keyframeSearchIdx;
            std::vector<float> keyframeSearchDist;
            pcl::KdTreeFLANN<PointType>::Ptr kdtreeKeyframes(new pcl::KdTreeFLANN<PointType>());
            kdtreeKeyframes->setInputCloud(cloudKeyPoses3D);
            int i = 0;
            
            // recover downsampled intensities
            for(auto& pt:cloudKeyPoses3DDS->points)
            {                
                kdtreeKeyframes->nearestKSearch(pt,1,keyframeSearchIdx,keyframeSearchDist); 
                pt.intensity = cloudKeyPoses6D->points[keyframeSearchIdx[0]].intensity;  
                pcl::io::savePCDFileBinary(saveKeyframeMapDirectory + "/corner" + std::to_string(i)+".pcd", *cornerCloudKeyFrames[pt.intensity]);
                pcl::io::savePCDFileBinary(saveKeyframeMapDirectory + "/surf" + std::to_string(i)+".pcd", *surfCloudKeyFrames[pt.intensity]);
                pose_file<<cloudKeyPoses6D->points[pt.intensity].x<<" "<<cloudKeyPoses6D->points[pt.intensity].y<<" "<<cloudKeyPoses6D->points[pt.intensity].z
                <<" "<<cloudKeyPoses6D->points[pt.intensity].roll<<" "<<cloudKeyPoses6D->points[pt.intensity].pitch<<" "<<cloudKeyPoses6D->points[pt.intensity].yaw
                << " " << i<<"\n";
                i++;
                if((i+1)%100 == 0) cout<<i<<" keyframes saved!"<<endl;
            }
            pose_file.close();
            cout<<"Keyframes Saving Finished!"<<endl;

        }
        res.success = true;
        doneSavingMap = true;
        return true;
    }
    void visualizeGlobalMapThread()
    {
        ros::Rate rate(0.2);
        
        while (ros::ok())
        {
            publishCloud(&pubKeyPoses, cloudKeyPoses3D, timeLidarInfoStamp, mapFrame);           
            publishGlobalMap();
            rate.sleep();
        }

        if (savePCD == false && saveKeyframeMap == false && savePose == false)
            return;

        kloam::save_mapRequest  req;
        kloam::save_mapResponse res;

        if (!doneSavingMap)
        {
            if(!saveMapService(req, res))   cout << "Fail to save map" << endl;
        }
    }

    void publishGlobalMap()
    {
        if (pubLidarCloudSurround.getNumSubscribers() == 0 || cloudKeyPoses3D->points.empty() == true)
        {
            return;
        }
        pcl::KdTreeFLANN<PointType>::Ptr kdtreeGlobalMap(new pcl::KdTreeFLANN<PointType>());;
        pcl::PointCloud<PointType>::Ptr globalMapKeyPoses(new pcl::PointCloud<PointType>());
        pcl::PointCloud<PointType>::Ptr globalMapKeyPosesDS(new pcl::PointCloud<PointType>());
        pcl::PointCloud<PointType>::Ptr globalMapKeyFrames(new pcl::PointCloud<PointType>());
        pcl::PointCloud<PointType>::Ptr globalMapKeyFramesDS(new pcl::PointCloud<PointType>());

        // kd-tree to find near key frames to visualize
        std::vector<int> pointSearchIndGlobalMap;
        std::vector<float> pointSearchSqDisGlobalMap;
        // search near key frames to visualize
        mtx.lock();
        kdtreeGlobalMap->setInputCloud(cloudKeyPoses3D);
        kdtreeGlobalMap->radiusSearch(cloudKeyPoses3D->back(), globalMapVisualizationSearchRadius, pointSearchIndGlobalMap, pointSearchSqDisGlobalMap, 0);
        mtx.unlock();

        for (int i = 0; i < (int)pointSearchIndGlobalMap.size(); ++i)
            globalMapKeyPoses->push_back(cloudKeyPoses3D->points[pointSearchIndGlobalMap[i]]);
        // downsample near selected key poses 
        pcl::VoxelGrid<PointType> downSizeFilterGlobalMapKeyPoses; // for global map visualization
        downSizeFilterGlobalMapKeyPoses.setLeafSize(globalMapVisualizationPoseDensity, globalMapVisualizationPoseDensity, globalMapVisualizationPoseDensity); // for global map visualization
        downSizeFilterGlobalMapKeyPoses.setInputCloud(globalMapKeyPoses);
        downSizeFilterGlobalMapKeyPoses.filter(*globalMapKeyPosesDS);
        // fix the keyframe downsample bug
        for(auto& pt : globalMapKeyPosesDS->points)
        {
            kdtreeGlobalMap->nearestKSearch(pt, 1, pointSearchIndGlobalMap, pointSearchSqDisGlobalMap);
            pt.intensity = cloudKeyPoses3D->points[pointSearchIndGlobalMap[0]].intensity;
        }

        // extract visualized and downsampled key frames
        // only for visualization
        for (int i = 0; i < (int)globalMapKeyPosesDS->size(); ++i)
        {
            if (pointDistance(globalMapKeyPosesDS->points[i], cloudKeyPoses3D->back()) > globalMapVisualizationSearchRadius)
                continue;
            int thisKeyInd = (int)globalMapKeyPosesDS->points[i].intensity;
            *globalMapKeyFrames += *transformPointCloud(cornerCloudKeyFrames[thisKeyInd],  &cloudKeyPoses6D->points[thisKeyInd]);
            *globalMapKeyFrames += *transformPointCloud(surfCloudKeyFrames[thisKeyInd],    &cloudKeyPoses6D->points[thisKeyInd]);
        }
        // downsample visualized points: why it is not working
        pcl::VoxelGrid<PointType> downSizeFilterGlobalMapKeyFrames; // for global map visualization
        downSizeFilterGlobalMapKeyFrames.setLeafSize(globalMapVisualizationLeafSize, globalMapVisualizationLeafSize, globalMapVisualizationLeafSize); // for global map visualization
        downSizeFilterGlobalMapKeyFrames.setInputCloud(globalMapKeyFrames);
        downSizeFilterGlobalMapKeyFrames.filter(*globalMapKeyFramesDS);        
        publishCloud(&pubLidarCloudSurround, globalMapKeyFramesDS, timeLidarInfoStamp, mapFrame);

    }

    void loopClosureThread()
    {
        if (loopClosureEnableFlag == false )
            return;

        ros::Rate rate(loopClosureFrequency);
        while (ros::ok())
        {
            rate.sleep();
            if (!localizationMode) 
            {
                performLoopClosure();
                visualizeLoopClosure();
            }
            else if (temporaryMappingMode)
            {
                tryMapMerging();
            }
            
        }
    }

    void tryMapMerging()
    {
        // cout<<"try map merging"<<endl;
        TicToc icp_match;
        if (!temporaryMappingMode || goodToMergeMap) // wait for the actual map merging to complete!
            return;

        mtx.lock();
        *copy_cloudKeyPoses3D = *cloudKeyPoses3D;
        *copy_cloudKeyPoses6D = *cloudKeyPoses6D;
        mtx.unlock();

        // find the closest history key frame
        std::vector<int> pointSearchIndLoop;
        std::vector<float> pointSearchSqDisLoop;
        // reset before usage!
        mtx.lock(); 
        kdtreeHistoryKeyPoses.reset(new pcl::KdTreeFLANN<PointType>());
        kdtreeHistoryKeyPoses->setInputCloud(copy_cloudKeyPoses3D);

        kdtreeHistoryKeyPoses->radiusSearch(temporaryCloudKeyPoses3D->back(), surroundingKeyframeSearchRadius, pointSearchIndLoop, pointSearchSqDisLoop, 0);

        if ( pointSearchIndLoop.empty()) 
        {
            // ROS_WARN("No nearby keyframe candidates");
            mtx.unlock();
            return;
        }
        int mergeIdxCurrent = temporaryCloudKeyPoses3D->size()-1;
        int mergeIdxPrev = pointSearchIndLoop[0];
        // extract cloud
        pcl::PointCloud<PointType>::Ptr cureKeyframeCloud(new pcl::PointCloud<PointType>());
        pcl::PointCloud<PointType>::Ptr prevKeyframeCloud(new pcl::PointCloud<PointType>());

        // //icp
        *cureKeyframeCloud += *transformPointCloud(temporaryCornerCloudKeyFrames[mergeIdxCurrent],&temporaryCloudKeyPoses6D->points[mergeIdxCurrent]);
        *cureKeyframeCloud += *transformPointCloud(temporarySurfCloudKeyFrames[mergeIdxCurrent],&temporaryCloudKeyPoses6D->points[mergeIdxCurrent]);
        // not so wise to find index-neighbors due to loop closing
        loopFindNearKeyframes(prevKeyframeCloud, mergeIdxPrev, historyKeyframeSearchNum); //25

        mtx.unlock();

        if (cureKeyframeCloud->size() < 300 || prevKeyframeCloud->size() < 1000)
            return;
        if (pubHistoryKeyFrames.getNumSubscribers() != 0)
            publishCloud(&pubHistoryKeyFrames, prevKeyframeCloud, timeLidarInfoStamp, mapFrame);

        // ICP Settings
        static pcl::IterativeClosestPoint<PointType, PointType> icp;
        icp.setMaxCorrespondenceDistance(historyKeyframeSearchRadius*2);
        icp.setMaximumIterations(100);
        icp.setTransformationEpsilon(1e-6);
        icp.setEuclideanFitnessEpsilon(1e-6);
        icp.setRANSACIterations(0);

        // Align clouds
        icp.setInputSource(cureKeyframeCloud);
        icp.setInputTarget(prevKeyframeCloud);
        pcl::PointCloud<PointType>::Ptr unused_result(new pcl::PointCloud<PointType>());
        icp.align(*unused_result);

        mergeNoise = icp.getFitnessScore();
        if (icp.hasConverged() == true && mergeNoise < historyKeyframeFitnessScore
            && temporaryCloudKeyPoses3D->points.size() > 10)
        {

        // cout<<"ICP score: "<<icp.getFitnessScore()<<endl;
            mergeCorrection = icp.getFinalTransformation();
            goodToMergeMap = true;
            // cout<<"ICP takes "<< icp_match.toc()<<" ms"<<endl;
            cout<<"Now it is okay to merge the temporary map"<<endl;
        }


        
    }

    void performLoopClosure()
    {
        if (cloudKeyPoses3D->points.empty() == true)
            return;

        mtx.lock();
        *copy_cloudKeyPoses3D = *cloudKeyPoses3D;
        *copy_cloudKeyPoses6D = *cloudKeyPoses6D;
        mtx.unlock();

        // find keys
        int loopKeyCur;
        int loopKeyPre;
        if (detectLoopClosureDistance(&loopKeyCur, &loopKeyPre) == false)
            return;

        // extract cloud
        pcl::PointCloud<PointType>::Ptr cureKeyframeCloud(new pcl::PointCloud<PointType>());
        pcl::PointCloud<PointType>::Ptr prevKeyframeCloud(new pcl::PointCloud<PointType>());
        {
            loopFindNearKeyframes(cureKeyframeCloud, loopKeyCur, 0);
            loopFindNearKeyframes(prevKeyframeCloud, loopKeyPre, historyKeyframeSearchNum);
            if (cureKeyframeCloud->size() < 300 || prevKeyframeCloud->size() < 1000)
                return;
            if (pubHistoryKeyFrames.getNumSubscribers() != 0)
                publishCloud(&pubHistoryKeyFrames, prevKeyframeCloud, timeLidarInfoStamp, mapFrame);
        }

        // ICP Settings
        static pcl::IterativeClosestPoint<PointType, PointType> icp;
        icp.setMaxCorrespondenceDistance(historyKeyframeSearchRadius*2);
        icp.setMaximumIterations(100);
        icp.setTransformationEpsilon(1e-6);
        icp.setEuclideanFitnessEpsilon(1e-6);
        icp.setRANSACIterations(0);

        // Align clouds
        icp.setInputSource(cureKeyframeCloud);
        icp.setInputTarget(prevKeyframeCloud);
        pcl::PointCloud<PointType>::Ptr unused_result(new pcl::PointCloud<PointType>());
        icp.align(*unused_result);

        if (icp.hasConverged() == false || icp.getFitnessScore() > historyKeyframeFitnessScore)
            return;
        
        // publish corrected cloud
        if (pubIcpKeyFrames.getNumSubscribers() != 0)
        {
            pcl::PointCloud<PointType>::Ptr closed_cloud(new pcl::PointCloud<PointType>());
            pcl::transformPointCloud(*cureKeyframeCloud, *closed_cloud, icp.getFinalTransformation());
            publishCloud(&pubIcpKeyFrames, closed_cloud, timeLidarInfoStamp, mapFrame);
        }

        // Get pose transformation
        float x, y, z, roll, pitch, yaw;
        Eigen::Affine3f correctionLidarFrame;
        correctionLidarFrame = icp.getFinalTransformation();
        // transform from world origin to wrong pose
        Eigen::Affine3f tWrong = pclPointToAffine3f(copy_cloudKeyPoses6D->points[loopKeyCur]);
        // transform from world origin to corrected pose
        Eigen::Affine3f tCorrect = correctionLidarFrame * tWrong;// pre-multiplying -> successive rotation about a fixed frame
        pcl::getTranslationAndEulerAngles (tCorrect, x, y, z, roll, pitch, yaw);
        gtsam::Pose3 poseFrom = Pose3(Rot3::RzRyRx(roll, pitch, yaw), Point3(x, y, z));
        gtsam::Pose3 poseTo = pclPointTogtsamPose3(copy_cloudKeyPoses6D->points[loopKeyPre]);
        gtsam::Vector Vector6(6);
        float noiseScore = icp.getFitnessScore();
        Vector6 << noiseScore, noiseScore, noiseScore, noiseScore, noiseScore, noiseScore;
        noiseModel::Diagonal::shared_ptr constraintNoise = noiseModel::Diagonal::Variances(Vector6);

        // Add pose constraint
        mtx.lock();
        loopIndexQueue.push_back(make_pair(loopKeyCur, loopKeyPre));
        loopPoseQueue.push_back(poseFrom.between(poseTo));
        loopNoiseQueue.push_back(constraintNoise);
        noiseVec.push_back(noiseScore);
        mtx.unlock();

        // add loop constriant
        // loopIndexContainer[loopKeyCur] = loopKeyPre;
        loopIndexContainer.insert(std::pair<int, int>(loopKeyCur, loopKeyPre)); // giseop for multimap
    }

    bool detectLoopClosureDistance(int *latestID, int *closestID)
    {
        int loopKeyCur = copy_cloudKeyPoses3D->size() - 1;
        int loopKeyPre = -1;

        // check loop constraint added before
        auto it = loopIndexContainer.find(loopKeyCur);
        if (it != loopIndexContainer.end())
            return false;

        // find the closest history key frame
        std::vector<int> pointSearchIndLoop;
        std::vector<float> pointSearchSqDisLoop;
        // reset before usage!
        kdtreeHistoryKeyPoses.reset(new pcl::KdTreeFLANN<PointType>());
        kdtreeHistoryKeyPoses->setInputCloud(copy_cloudKeyPoses3D);
        kdtreeHistoryKeyPoses->radiusSearch(copy_cloudKeyPoses3D->back(), historyKeyframeSearchRadius, pointSearchIndLoop, pointSearchSqDisLoop, 0);
        // cout<<copy_cloudKeyPoses6D->points[id].time-cloudKeyPoses6D->points[id].time<<" "<<copy_cloudKeyPoses6D->points[id].time-cloudInfoTime <<endl;
        for (int i = 0; i < (int)pointSearchIndLoop.size(); ++i)
        {
            int id = pointSearchIndLoop[i];
            
            if (abs(copy_cloudKeyPoses6D->points[id].time - cloudInfoTime) > historyKeyframeSearchTimeDiff)
            {
                loopKeyPre = id;
                break;
            }
        }

        if (loopKeyPre == -1 || loopKeyCur == loopKeyPre)
            return false;

        *latestID = loopKeyCur;
        *closestID = loopKeyPre;

        return true;
    }

    void loopFindNearKeyframes(pcl::PointCloud<PointType>::Ptr& nearKeyframes, const int& key, const int& searchNum, bool doFiltering = true)
    {
        // extract near keyframes
        nearKeyframes->clear();
        int cloudSize = copy_cloudKeyPoses6D->size();
        for (int i = -searchNum; i <= searchNum; ++i)
        {
            int keyNear = key + i;
            if (keyNear < 0 || keyNear >= cloudSize )
                continue;
            // it may happen in temporary mapping mode
            float dist = pointDistance(copy_cloudKeyPoses3D->points[key],copy_cloudKeyPoses3D->points[keyNear]);
            if (  dist > surroundingKeyframeSearchRadius)
                continue;
            *nearKeyframes += *transformPointCloud(cornerCloudKeyFrames[keyNear], &copy_cloudKeyPoses6D->points[keyNear]);  
            *nearKeyframes += *transformPointCloud(surfCloudKeyFrames[keyNear],   &copy_cloudKeyPoses6D->points[keyNear]);
        }

        if (nearKeyframes->empty())
            return;

        // downsample near keyframes
        if (doFiltering){
            pcl::PointCloud<PointType>::Ptr cloud_temp(new pcl::PointCloud<PointType>());
            downSizeFilterICP.setInputCloud(nearKeyframes);
            downSizeFilterICP.filter(*cloud_temp);
            *nearKeyframes = *cloud_temp;
        }
    }
    // copied to the original, so that reloc part can use this func
    void loopFindNearKeyframesReloc(pcl::PointCloud<PointType>::Ptr& nearKeyframes, const int& key, const int& searchNum, bool doFiltering = true)
    {
        // extract near keyframes
        nearKeyframes->clear();
        int cloudSize = cloudKeyPoses6D->size();
        for (int i = -searchNum; i <= searchNum; ++i)
        {
            int keyNear = key + i;
            if (keyNear < 0 || keyNear >= cloudSize )
                continue;
                // copied to the original, so that reloc part can use this func too
            *nearKeyframes += *transformPointCloud(cornerCloudKeyFrames[keyNear], &cloudKeyPoses6D->points[keyNear]);  
            *nearKeyframes += *transformPointCloud(surfCloudKeyFrames[keyNear],   &cloudKeyPoses6D->points[keyNear]);
        }

        if (nearKeyframes->empty())
            return;

        // downsample near keyframes
        if (doFiltering){
            pcl::PointCloud<PointType>::Ptr cloud_temp(new pcl::PointCloud<PointType>());
            downSizeFilterICP.setInputCloud(nearKeyframes);
            downSizeFilterICP.filter(*cloud_temp);
            *nearKeyframes = *cloud_temp;
        }
    }

    void visualizeLoopClosure()
    {
        if (loopIndexContainer.empty())
            return;
        
        visualization_msgs::MarkerArray markerArray;
        // loop nodes
        visualization_msgs::Marker markerNode;
        markerNode.header.frame_id = mapFrame;
        markerNode.header.stamp = timeLidarInfoStamp;
        markerNode.action = visualization_msgs::Marker::ADD;
        markerNode.type = visualization_msgs::Marker::SPHERE_LIST;
        markerNode.ns = "loop_nodes";
        markerNode.id = 0;
        markerNode.pose.orientation.w = 1;
        markerNode.scale.x = 0.3; markerNode.scale.y = 0.3; markerNode.scale.z = 0.3; 
        markerNode.color.r = 0; markerNode.color.g = 0.8; markerNode.color.b = 1;
        markerNode.color.a = 1;
        // loop edges
        visualization_msgs::Marker markerEdge;
        markerEdge.header.frame_id = mapFrame;
        markerEdge.header.stamp = timeLidarInfoStamp;
        markerEdge.action = visualization_msgs::Marker::ADD;
        markerEdge.type = visualization_msgs::Marker::LINE_LIST;
        markerEdge.ns = "loop_edges";
        markerEdge.id = 1;
        markerEdge.pose.orientation.w = 1;
        markerEdge.scale.x = 0.1;
        markerEdge.color.r = 0.9; markerEdge.color.g = 0.9; markerEdge.color.b = 0;
        markerEdge.color.a = 1;

        for (auto it = loopIndexContainer.begin(); it != loopIndexContainer.end(); ++it)
        {
            int key_cur = it->first;
            int key_pre = it->second;
            geometry_msgs::Point p;
            p.x = copy_cloudKeyPoses6D->points[key_cur].x;
            p.y = copy_cloudKeyPoses6D->points[key_cur].y;
            p.z = copy_cloudKeyPoses6D->points[key_cur].z;
            markerNode.points.push_back(p);
            markerEdge.points.push_back(p);
            p.x = copy_cloudKeyPoses6D->points[key_pre].x;
            p.y = copy_cloudKeyPoses6D->points[key_pre].y;
            p.z = copy_cloudKeyPoses6D->points[key_pre].z;
            markerNode.points.push_back(p);
            markerEdge.points.push_back(p);
        }

        markerArray.markers.push_back(markerNode);
        markerArray.markers.push_back(markerEdge);
        pubLoopConstraintEdge.publish(markerArray);
        // ROS_INFO_STREAM("Loop noise: "<<noiseVec[noiseVec.size()-1]);

    }

    pcl::PointCloud<PointType>::Ptr transformPointCloud(pcl::PointCloud<PointType>::Ptr cloudIn, Eigen::Affine3f transCur)
    {
        int cloudSize = cloudIn->size();
        pcl::PointCloud<PointType>::Ptr cloudOut(new pcl::PointCloud<PointType>());
        cloudOut->resize(cloudSize);
        #pragma omp parallel for num_threads(numberOfCores)
        for (int i = 0; i < cloudSize; ++i)
        {
            const auto &pointFrom = cloudIn->points[i];
            cloudOut->points[i].x = transCur(0,0) * pointFrom.x + transCur(0,1) * pointFrom.y + transCur(0,2) * pointFrom.z + transCur(0,3);
            cloudOut->points[i].y = transCur(1,0) * pointFrom.x + transCur(1,1) * pointFrom.y + transCur(1,2) * pointFrom.z + transCur(1,3);
            cloudOut->points[i].z = transCur(2,0) * pointFrom.x + transCur(2,1) * pointFrom.y + transCur(2,2) * pointFrom.z + transCur(2,3);
            cloudOut->points[i].intensity = pointFrom.intensity;
        }
        return cloudOut;
    }

    void initialpose_callback(const rvizPoseType& pose_msg) {
        ROS_INFO("initial pose received!!");
        std::lock_guard<std::mutex> lock(pose_estimator_mutex);
        poseEstVec.push_back(pose_msg);
        if (relocByRviz()){
            ROS_INFO("Got pose estimate");
            poseGuessFromRvizAvailable = true;
        }
    }





    bool relocByRviz() {
        if (!poseEstVec.empty())
        {
            // use mutex for copy process to prevent concurrent change to the cloudKeyPoses3D,seems no need
            Eigen::Affine3f transBackup;
            mtx.lock(); 
            transBackup = pcl::getTransformation(transformTobeMapped[3], transformTobeMapped[4], transformTobeMapped[5], 
                                                            transformTobeMapped[0], transformTobeMapped[1], transformTobeMapped[2]);
            // *copy2_cloudKeyPoses3D = *cloudKeyPoses3D;
            // *copy2_cloudKeyPoses6D = *cloudKeyPoses6D;
            mtx.unlock();

            kdtreeHistoryKeyPoses.reset(new pcl::KdTreeFLANN<PointType>());
            kdtreeHistoryKeyPoses->setInputCloud(cloudKeyPoses3D);
            PointType pt;
            vector<int> pointSearchIndLoop;
            vector<float> pointSearchSqDisLoop;
            rvizPoseType pose_msg = poseEstVec.back();
            const auto& p = pose_msg->pose.pose.position;
            const auto& q = pose_msg->pose.pose.orientation;
            tf::Quaternion q_tf(q.x,q.y,q.z,q.w);
            double roll,pitch,yaw;
            tf::Matrix3x3(q_tf).getRPY(roll,pitch,yaw);
            Eigen::Affine3f affineGuess = pcl::getTransformation(p.x, p.y, p.z, (float)roll,(float)pitch,(float)yaw);
            relocCorrection = affineGuess*transBackup.inverse(); 
            poseEstVec.clear();
            return true;     
        }
        return false;
    }





    void updateInitialGuess()
    {
        static Eigen::Affine3f lastImuTransformation;
        static Eigen::Affine3f lastLidarTransformation;
        
        if(localizationMode && poseGuessFromRvizAvailable)
        {
            Eigen::Affine3f tWrong = trans2Affine3f(transformTobeMapped);
            Eigen::Affine3f tCorrect = relocCorrection*tWrong;
            pcl::getTranslationAndEulerAngles(tCorrect, transformTobeMapped[3], transformTobeMapped[4], transformTobeMapped[5], 
                                                            transformTobeMapped[0], transformTobeMapped[1], transformTobeMapped[2]);
            for(int i = 0;i < 6;i++){
                rvizGuess[i] = transformTobeMapped[i];
                transformBeforeMapped[i] = transformTobeMapped[i];
            }
            printTrans("The reloc pose given by rviz: ",transformTobeMapped);
            poseGuessFromRvizAvailable = false;
            return;
        }
        // initialization 
        if (localizationMode && !relocSuccess)
        {
                //  use ground truth only for relocalization!
                int tryRuns = 3;
                static int count = 0;
                if (!gtVec.empty() && count < tryRuns)
                {
                    odometryMsgToTrans(gtVec.back(),transformTobeMapped);
                    for(int i=0;i<6;i++){
                        transformBeforeMapped[i] = transformTobeMapped[i];
                    }
                    printTrans("Initial: ",transformTobeMapped);
                    relocSuccess = true;
                }
                else if (count < tryRuns)
                {
                    count++;
                    ROS_INFO("Initializing: %d",count);
                }
                else
                {
                    for(int i=0;i<6;i++){
                        transformTobeMapped[i] = initialGuess[i];
                        transformBeforeMapped[i] = initialGuess[i];
                    }
                    printTrans("Initial: ",transformTobeMapped);
                    relocSuccess = true;
                }
                return;          
        }

        
        float arrayTmp[6];
        affine_lidar_to_map = affine_odom_to_map*affine_lidar_to_odom;
        Affine3f2Trans(affine_lidar_to_map, arrayTmp);
        
        for (int i=0;i<6;i++)                
        {
            transformTobeMapped[i] = arrayTmp[i];
            transformBeforeMapped[i] = transformTobeMapped[i];
        }
        return;
    }

    void extractNearby()
    {
        if (temporaryMappingMode)
        {
            pcl::copyPointCloud(*temporaryCloudKeyPoses3D, *keyPosesTarget3D);
            pcl::copyPointCloud(*temporaryCloudKeyPoses6D, *keyPosesTarget6D);
        }
        else
        {
             pcl::copyPointCloud(*cloudKeyPoses3D, *keyPosesTarget3D);
             pcl::copyPointCloud(*cloudKeyPoses6D, *keyPosesTarget6D);
        }
        if (keyPosesTarget3D->points.empty() == true) 
            return; 
        pcl::PointCloud<PointType>::Ptr surroundingKeyPoses(new pcl::PointCloud<PointType>());
        pcl::PointCloud<PointType>::Ptr surroundingKeyPosesDS(new pcl::PointCloud<PointType>());
        std::vector<int> pointSearchInd;
        std::vector<float> pointSearchSqDis;

        // extract all the nearby key poses and downsample them
        kdtreeSurroundingKeyPoses->setInputCloud(keyPosesTarget3D); // create kd-tree
        PointType pt;
        pt.x=transformTobeMapped[3];
        pt.y=transformTobeMapped[4];
        pt.z=transformTobeMapped[5];
        
        kdtreeSurroundingKeyPoses->radiusSearch(pt, (double)surroundingKeyframeSearchRadius, pointSearchInd, pointSearchSqDis);

        bool temporary = false;
        if (pointSearchInd.empty()) 
        {
            cout<<pt.x<< " "<<pt.y <<"; keypoints: "<<keyPosesTarget3D->points[0].x<<" "<< keyPosesTarget3D->points[0].y<<endl;
            ROS_WARN("No nearby keyposes within %f meters",surroundingKeyframeSearchRadius);
            temporary = true;
        }
        // else if (pointSearchSqDis[0] > starttemporaryMappingDistThre) // it is supposed to be used for only init. process
        // { 
        //     cout<<"dist: "<<pointSearchSqDis[0]<<endl;
        //     temporary = true;
        // }

        if (temporary && localizationMode && !temporaryMappingMode)
        {
            // normally it only happens when robot start in an previously-unmapped position
            pcl::copyPointCloud(*temporaryCloudKeyPoses3D, *keyPosesTarget3D);
            pcl::copyPointCloud(*temporaryCloudKeyPoses6D, *keyPosesTarget6D);
            temporaryMappingMode = true;
            startTemporaryMappingIndex = temporaryCloudKeyPoses3D->size();
            cout<<"Entering temporary mapping mode due to entering an un-mapped place"<<endl;
            frameTobeAbandoned = true;
            return;
        }
        for (int i = 0; i < (int)pointSearchInd.size(); ++i)
        {
            int id = pointSearchInd[i];
            surroundingKeyPoses->push_back(keyPosesTarget3D->points[id]);
        }

        // downsampling is important especially at places where trajectories overlap when doing slam
        
        downSizeFilterSurroundingKeyPoses.setInputCloud(surroundingKeyPoses);
        downSizeFilterSurroundingKeyPoses.filter(*surroundingKeyPosesDS);

        for(auto& pt : surroundingKeyPosesDS->points) // recover the intensity field averaged by voxel filter
        {
            kdtreeSurroundingKeyPoses->nearestKSearch(pt, 1, pointSearchInd, pointSearchSqDis);
            pt.intensity = keyPosesTarget3D->points[pointSearchInd[0]].intensity;
        }

        if (!localizationMode)
        {
            // also extract some latest key frames in case the robot rotates in one position
            // more recent ones matches better with current frames
            int numPoses = cloudKeyPoses3D->size();
            for (int i = numPoses-1; i >= 0; --i)
            {
                if (cloudInfoTime - cloudKeyPoses6D->points[i].time < 10.0)
                    surroundingKeyPosesDS->push_back(cloudKeyPoses3D->points[i]);
                else
                    break;
            }
        }
        extractCloud(surroundingKeyPosesDS);
    }

    void extractCloud(pcl::PointCloud<PointType>::Ptr cloudToExtract)
    {
        // fuse the map
        lidarCloudCornerFromMap->clear();
        lidarCloudSurfFromMap->clear(); 
        // TicToc transPC;
        for (int i = 0; i < (int)cloudToExtract->size(); ++i)
        {
            if(!localizationMode){ // for localization mode, .back is not the most recent!
                if (pointDistance(cloudToExtract->points[i], cloudKeyPoses3D->back()) > surroundingKeyframeSearchRadius)
                    continue;
            }
            int thisKeyInd = (int)cloudToExtract->points[i].intensity;

            if (temporaryMappingMode)
            {
                *lidarCloudCornerFromMap += *transformPointCloud(temporaryCornerCloudKeyFrames[thisKeyInd],  &keyPosesTarget6D->points[thisKeyInd]);
                *lidarCloudSurfFromMap   += *transformPointCloud(temporarySurfCloudKeyFrames[thisKeyInd],    &keyPosesTarget6D->points[thisKeyInd]);
            }
            else
            {
                *lidarCloudCornerFromMap += *transformPointCloud(cornerCloudKeyFrames[thisKeyInd],  &keyPosesTarget6D->points[thisKeyInd]);
                *lidarCloudSurfFromMap   += *transformPointCloud(surfCloudKeyFrames[thisKeyInd],    &keyPosesTarget6D->points[thisKeyInd]);
            }          
        }
        
        // Downsample the surrounding corner key frames (or map)
        downSizeFilterCorner.setInputCloud(lidarCloudCornerFromMap);
        downSizeFilterCorner.filter(*lidarCloudCornerFromMapDS);
        lidarCloudCornerFromMapDSNum = lidarCloudCornerFromMapDS->size();
        // Downsample the surrounding surf key frames (or map)
        downSizeFilterSurf.setInputCloud(lidarCloudSurfFromMap);
        downSizeFilterSurf.filter(*lidarCloudSurfFromMapDS);
        lidarCloudSurfFromMapDSNum = lidarCloudSurfFromMapDS->size();
        
    }


    void downsampleCurrentScan()
    {
        if (keyPosesTarget3D->points.empty())
            return;
        // Downsample cloud from current scan
        lidarCloudCornerLastDS->clear();
        downSizeFilterCorner.setInputCloud(lidarCloudCornerLast);
        downSizeFilterCorner.filter(*lidarCloudCornerLastDS);
        lidarCloudCornerLastDSNum = lidarCloudCornerLastDS->size();
        // cout<<"After sampling: "<<lidarCloudCornerLastDSNum<<endl;
        lidarCloudSurfLastDS->clear();
        downSizeFilterSurf.setInputCloud(lidarCloudSurfLast);
        downSizeFilterSurf.filter(*lidarCloudSurfLastDS);
        lidarCloudSurfLastDSNum = lidarCloudSurfLastDS->size();
    }

    void cornerOptimization(int iterCount)
    {
        tmpEdgeErrorCloud.reset(new pcl::PointCloud<PointType>());
        affine_lidar_to_map = trans2Affine3f(transformTobeMapped);

        // #pragma omp parallel for num_threads(numberOfCores) // runtime error, don't use it!
        for (int i = 0; i < lidarCloudCornerLastDSNum; i++)
        {
            PointType pointOri, pointSel, coeff;
            std::vector<int> pointSearchInd;
            std::vector<float> pointSearchSqDis;

            pointOri = lidarCloudCornerLastDS->points[i];
            pointAssociateToMap(&pointOri, &pointSel);
            kdtreeCornerFromMap->nearestKSearch(pointSel, 5, pointSearchInd, pointSearchSqDis);

            cv::Mat matA1(3, 3, CV_32F, cv::Scalar::all(0));
            cv::Mat matD1(1, 3, CV_32F, cv::Scalar::all(0));
            cv::Mat matV1(3, 3, CV_32F, cv::Scalar::all(0));
                    
            if (pointSearchSqDis[4] < 1.0) {
                float cx = 0, cy = 0, cz = 0;
                for (int j = 0; j < 5; j++) {
                    cx += lidarCloudCornerFromMapDS->points[pointSearchInd[j]].x;
                    cy += lidarCloudCornerFromMapDS->points[pointSearchInd[j]].y;
                    cz += lidarCloudCornerFromMapDS->points[pointSearchInd[j]].z;
                }
                cx /= 5; cy /= 5;  cz /= 5;

                float a11 = 0, a12 = 0, a13 = 0, a22 = 0, a23 = 0, a33 = 0;
                for (int j = 0; j < 5; j++) {
                    float ax = lidarCloudCornerFromMapDS->points[pointSearchInd[j]].x - cx;
                    float ay = lidarCloudCornerFromMapDS->points[pointSearchInd[j]].y - cy;
                    float az = lidarCloudCornerFromMapDS->points[pointSearchInd[j]].z - cz;

                    a11 += ax * ax; a12 += ax * ay; a13 += ax * az;
                    a22 += ay * ay; a23 += ay * az;
                    a33 += az * az;
                }
                a11 /= 5; a12 /= 5; a13 /= 5; a22 /= 5; a23 /= 5; a33 /= 5;

                matA1.at<float>(0, 0) = a11; matA1.at<float>(0, 1) = a12; matA1.at<float>(0, 2) = a13;
                matA1.at<float>(1, 0) = a12; matA1.at<float>(1, 1) = a22; matA1.at<float>(1, 2) = a23;
                matA1.at<float>(2, 0) = a13; matA1.at<float>(2, 1) = a23; matA1.at<float>(2, 2) = a33;

                cv::eigen(matA1, matD1, matV1);

                if (matD1.at<float>(0, 0) > 3 * matD1.at<float>(0, 1)) {

                    float x0 = pointSel.x;
                    float y0 = pointSel.y;
                    float z0 = pointSel.z;
                    float x1 = cx + 0.1 * matV1.at<float>(0, 0);
                    float y1 = cy + 0.1 * matV1.at<float>(0, 1);
                    float z1 = cz + 0.1 * matV1.at<float>(0, 2);
                    float x2 = cx - 0.1 * matV1.at<float>(0, 0);
                    float y2 = cy - 0.1 * matV1.at<float>(0, 1);
                    float z2 = cz - 0.1 * matV1.at<float>(0, 2);

                    float a012 = sqrt(((x0 - x1)*(y0 - y2) - (x0 - x2)*(y0 - y1)) * ((x0 - x1)*(y0 - y2) - (x0 - x2)*(y0 - y1)) 
                                    + ((x0 - x1)*(z0 - z2) - (x0 - x2)*(z0 - z1)) * ((x0 - x1)*(z0 - z2) - (x0 - x2)*(z0 - z1)) 
                                    + ((y0 - y1)*(z0 - z2) - (y0 - y2)*(z0 - z1)) * ((y0 - y1)*(z0 - z2) - (y0 - y2)*(z0 - z1)));

                    float l12 = sqrt((x1 - x2)*(x1 - x2) + (y1 - y2)*(y1 - y2) + (z1 - z2)*(z1 - z2));

                    float la = ((y1 - y2)*((x0 - x1)*(y0 - y2) - (x0 - x2)*(y0 - y1)) 
                              + (z1 - z2)*((x0 - x1)*(z0 - z2) - (x0 - x2)*(z0 - z1))) / a012 / l12;

                    float lb = -((x1 - x2)*((x0 - x1)*(y0 - y2) - (x0 - x2)*(y0 - y1)) 
                               - (z1 - z2)*((y0 - y1)*(z0 - z2) - (y0 - y2)*(z0 - z1))) / a012 / l12;

                    float lc = -((x1 - x2)*((x0 - x1)*(z0 - z2) - (x0 - x2)*(z0 - z1)) 
                               + (y1 - y2)*((y0 - y1)*(z0 - z2) - (y0 - y2)*(z0 - z1))) / a012 / l12;

                    float ld2 = a012 / l12;

                    float s = 1 - 0.9 * fabs(ld2);

                    coeff.x = s * la;
                    coeff.y = s * lb;
                    coeff.z = s * lc;
                    coeff.intensity = s * ld2;

                    if (s > 0.1) 
                    {
                        lidarCloudOriCornerVec[i] = pointOri;
                        coeffSelCornerVec[i] = coeff;
                        lidarCloudOriCornerFlag[i] = true;
                        PointType p;
                        // get the closest map correspondence
                        p.x = lidarCloudCornerFromMapDS->points[pointSearchInd[0]].x;
                        p.y = lidarCloudCornerFromMapDS->points[pointSearchInd[0]].y;
                        p.z = lidarCloudCornerFromMapDS->points[pointSearchInd[0]].z;
                        p.intensity = fabs(ld2);
                        tmpEdgeErrorCloud->push_back(p);
                        if (p.intensity > maxEdgeIntensity) maxEdgeIntensity = p.intensity;
                        if (p.intensity > maxIntensity) maxIntensity = p.intensity;
                    }
                }
            }
        }
    }

    void surfOptimization(int iterCount)
    {
        tmpSurfErrorCloud.reset(new pcl::PointCloud<PointType>());
        affine_lidar_to_map = trans2Affine3f(transformTobeMapped);

        for (int i = 0; i < lidarCloudSurfLastDSNum; i++)
        {
            PointType pointOri, pointSel, coeff;
            std::vector<int> pointSearchInd;
            std::vector<float> pointSearchSqDis;

            pointOri = lidarCloudSurfLastDS->points[i];
            pointAssociateToMap(&pointOri, &pointSel); 
            kdtreeSurfFromMap->nearestKSearch(pointSel, 5, pointSearchInd, pointSearchSqDis);

            Eigen::Matrix<float, 5, 3> matA0;
            Eigen::Matrix<float, 5, 1> matB0;
            Eigen::Vector3f matX0;

            matA0.setZero();
            matB0.fill(-1);
            matX0.setZero();

            if (pointSearchSqDis[4] < 1.0) {
                for (int j = 0; j < 5; j++) 
                {
                    matA0(j, 0) = lidarCloudSurfFromMapDS->points[pointSearchInd[j]].x;
                    matA0(j, 1) = lidarCloudSurfFromMapDS->points[pointSearchInd[j]].y;
                    matA0(j, 2) = lidarCloudSurfFromMapDS->points[pointSearchInd[j]].z;
                }
                // why Ax = B, means x is the unit normal vector of the plane?
                matX0 = matA0.colPivHouseholderQr().solve(matB0);

                float pa = matX0(0, 0);
                float pb = matX0(1, 0);
                float pc = matX0(2, 0);
                float pd = 1;

                float ps = sqrt(pa * pa + pb * pb + pc * pc);
                pa /= ps; pb /= ps; pc /= ps; pd /= ps;

                bool planeValid = true;
                for (int j = 0; j < 5; j++) {
                    if (fabs(pa * lidarCloudSurfFromMapDS->points[pointSearchInd[j]].x +
                             pb * lidarCloudSurfFromMapDS->points[pointSearchInd[j]].y +
                             pc * lidarCloudSurfFromMapDS->points[pointSearchInd[j]].z + pd) > 0.2) {
                        planeValid = false;
                        break;
                    }
                }

                if (planeValid) {
                    float pd2 = pa * pointSel.x + pb * pointSel.y + pc * pointSel.z + pd;

                    float s = 1 - 0.9 * fabs(pd2) / sqrt(sqrt(pointSel.x * pointSel.x
                            + pointSel.y * pointSel.y + pointSel.z * pointSel.z));

                    coeff.x = s * pa;
                    coeff.y = s * pb;
                    coeff.z = s * pc;
                    coeff.intensity = s * pd2;

                    if (s > 0.1) {
                        lidarCloudOriSurfVec[i] = pointOri;
                        coeffSelSurfVec[i] = coeff;
                        lidarCloudOriSurfFlag[i] = true;
                        PointType p;
                        // get the closest map correspondence
                        p.x = lidarCloudSurfFromMapDS->points[pointSearchInd[0]].x;
                        p.y = lidarCloudSurfFromMapDS->points[pointSearchInd[0]].y;
                        p.z = lidarCloudSurfFromMapDS->points[pointSearchInd[0]].z;
                        p.intensity = pd2;
                        tmpSurfErrorCloud->push_back(p);
                        if (p.intensity > maxSurfIntensity) maxSurfIntensity = p.intensity;
                        if (p.intensity > maxIntensity) maxIntensity = p.intensity;
                        
                    }
                }
            }
        }
    }

    void combineOptimizationCoeffs()
    {
        // combine corner coeffs
        edgePointCorrNum = 0;
        surfPointCorrNum = 0;
        for (int i = 0; i < lidarCloudCornerLastDSNum; ++i){
            if (lidarCloudOriCornerFlag[i] == true){
                lidarCloudOri->push_back(lidarCloudOriCornerVec[i]);
                coeffSel->push_back(coeffSelCornerVec[i]);
                edgePointCorrNum++;
            }
        }
        // combine surf coeffs
        for (int i = 0; i < lidarCloudSurfLastDSNum; ++i){
            if (lidarCloudOriSurfFlag[i] == true){
                lidarCloudOri->push_back(lidarCloudOriSurfVec[i]);
                coeffSel->push_back(coeffSelSurfVec[i]);
                surfPointCorrNum++;
            }
        }
        // reset flag for next iteration
        std::fill(lidarCloudOriCornerFlag.begin(), lidarCloudOriCornerFlag.end(), false);
        std::fill(lidarCloudOriSurfFlag.begin(), lidarCloudOriSurfFlag.end(), false);
    }

    bool LMOptimization(int iterCount)
    {
        // float s1 = sin(transformTobeMapped[0]);
        // float c1 = cos(transformTobeMapped[0]);
        // float s2 = sin(transformTobeMapped[1]);
        // float c2 = cos(transformTobeMapped[1]);
        // float s3 = sin(transformTobeMapped[2]);
        // float c3 = cos(transformTobeMapped[2]);

        // int lidarCloudSelNum = lidarCloudOri->size();
        // if (lidarCloudSelNum < 50) {
        //     return false;
        // }

        // cv::Mat matA(lidarCloudSelNum, 6, CV_32F, cv::Scalar::all(0));
        // cv::Mat matAt(6, lidarCloudSelNum, CV_32F, cv::Scalar::all(0));
        // cv::Mat matAtA(6, 6, CV_32F, cv::Scalar::all(0));
        // cv::Mat matB(lidarCloudSelNum, 1, CV_32F, cv::Scalar::all(0));
        // cv::Mat matAtB(6, 1, CV_32F, cv::Scalar::all(0));
        // cv::Mat matX(6, 1, CV_32F, cv::Scalar::all(0));

        // PointType pointOri, coeff;
        // notice  the subscript in wiki is the sequence, not x y z!!!
        // for (int i = 0; i < lidarCloudSelNum; i++) {
        //     pointOri.x = lidarCloudOri->points[i].x;
        //     pointOri.y = lidarCloudOri->points[i].y;
        //     pointOri.z = lidarCloudOri->points[i].z;
        //     coeff.x = coeffSel->points[i].x;
        //     coeff.y = coeffSel->points[i].y;
        //     coeff.z = coeffSel->points[i].z;
        //     coeff.intensity = coeffSel->points[i].intensity;
        //     // type it right the 3rd time. but why so slow?
        //     float arx = ((-s1*s3+c1*s2*c3)*pointOri.x + (-s1*c3-c1*s2*s3)*pointOri.y - c1*c2*pointOri.z) * coeff.y
        //               + ((c1*s3+s1*s2*c3)*pointOri.x + (c1*c3-s1*s2*s3)*pointOri.y - s1*c2*pointOri.z) * coeff.z;

        //     float ary = (-s2*c3*pointOri.x + s2*s3*pointOri.y + c2*pointOri.z) * coeff.x
        //               + (s1*c2*c3*pointOri.x - s1*c2*s3*pointOri.y + s1*s2*pointOri.z) * coeff.y
        //               + (-c1*c2*c3*pointOri.x + c1*c2*s3*pointOri.y - c1*s2*pointOri.z) * coeff.z;

        //     float arz = (-c2*s3*pointOri.x - c2*c3*pointOri.y)*coeff.x
        //               + ((c1*c3-s1*s2*s3)*pointOri.x + (-c1*s3-s1*s2*c3)*pointOri.y) * coeff.y
        //               + ((s1*c3 + c1*s2*s3)*pointOri.x + (-s1*s3+c1*s2*c3)*pointOri.y)*coeff.z;
        //     matA.at<float>(i, 0) = arx;
        //     matA.at<float>(i, 1) = ary;
        //     matA.at<float>(i, 2) = arz;
        //     matA.at<float>(i, 3) = coeff.x;
        //     matA.at<float>(i, 4) = coeff.y;
        //     matA.at<float>(i, 5) = coeff.z;
        //     matB.at<float>(i, 0) = -coeff.intensity; // -f, no 0.05 here,same in LOAM
        // }

        // this way much faster, why?
       // lidar -> camera
        float srx = sin(transformTobeMapped[1]);
        float crx = cos(transformTobeMapped[1]);
        float sry = sin(transformTobeMapped[2]);
        float cry = cos(transformTobeMapped[2]);
        float srz = sin(transformTobeMapped[0]);
        float crz = cos(transformTobeMapped[0]);

        int lidarCloudSelNum = lidarCloudOri->size();

        cv::Mat matA(lidarCloudSelNum, 6, CV_32F, cv::Scalar::all(0));
        cv::Mat matAt(6, lidarCloudSelNum, CV_32F, cv::Scalar::all(0));
        cv::Mat matAtA(6, 6, CV_32F, cv::Scalar::all(0));
        cv::Mat matB(lidarCloudSelNum, 1, CV_32F, cv::Scalar::all(0));
        cv::Mat matAtB(6, 1, CV_32F, cv::Scalar::all(0));
        cv::Mat matX(6, 1, CV_32F, cv::Scalar::all(0));

        PointType pointOri, coeff;
        mapRegistrationError.clear(); // only get the last iteration error
        for (int i = 0; i < lidarCloudSelNum; i++) {
            mapRegistrationError.push_back(fabs(coeffSel->points[i].intensity));
            // lidar -> camera
            pointOri.x = lidarCloudOri->points[i].y;
            pointOri.y = lidarCloudOri->points[i].z;
            pointOri.z = lidarCloudOri->points[i].x;
            // lidar -> camera
            coeff.x = coeffSel->points[i].y;
            coeff.y = coeffSel->points[i].z;
            coeff.z = coeffSel->points[i].x;
            coeff.intensity = coeffSel->points[i].intensity;
            // in camera
            float arx = (crx*sry*srz*pointOri.x + crx*crz*sry*pointOri.y - srx*sry*pointOri.z) * coeff.x
                      + (-srx*srz*pointOri.x - crz*srx*pointOri.y - crx*pointOri.z) * coeff.y
                      + (crx*cry*srz*pointOri.x + crx*cry*crz*pointOri.y - cry*srx*pointOri.z) * coeff.z;

            float ary = ((cry*srx*srz - crz*sry)*pointOri.x 
                      + (sry*srz + cry*crz*srx)*pointOri.y + crx*cry*pointOri.z) * coeff.x
                      + ((-cry*crz - srx*sry*srz)*pointOri.x 
                      + (cry*srz - crz*srx*sry)*pointOri.y - crx*sry*pointOri.z) * coeff.z;

            float arz = ((crz*srx*sry - cry*srz)*pointOri.x + (-cry*crz-srx*sry*srz)*pointOri.y)*coeff.x
                      + (crx*crz*pointOri.x - crx*srz*pointOri.y) * coeff.y
                      + ((sry*srz + cry*crz*srx)*pointOri.x + (crz*sry-cry*srx*srz)*pointOri.y)*coeff.z;
            // camera -> lidar
            matA.at<float>(i, 0) = arz;
            matA.at<float>(i, 1) = arx;
            matA.at<float>(i, 2) = ary;
            matA.at<float>(i, 3) = coeff.z;
            matA.at<float>(i, 4) = coeff.x;
            matA.at<float>(i, 5) = coeff.y;
            matB.at<float>(i, 0) = -coeff.intensity;
        }
        cv::transpose(matA, matAt);
        matAtA = matAt * matA;
        matAtB = matAt * matB;
        cv::solve(matAtA, matAtB, matX, cv::DECOMP_QR);

        if (iterCount == 0) {

            cv::Mat matE(1, 6, CV_32F, cv::Scalar::all(0));
            cv::Mat matV(6, 6, CV_32F, cv::Scalar::all(0));
            cv::Mat matV2(6, 6, CV_32F, cv::Scalar::all(0));

            cv::eigen(matAtA, matE, matV);
            matV.copyTo(matV2);

            isDegenerate = false;
            float eignThre[6] = {100, 100, 100, 100, 100, 100};
            for (int i = 5; i >= 0; i--) { 
                if (matE.at<float>(0, i) < eignThre[i]) {
                    for (int j = 0; j < 6; j++) {
                        matV2.at<float>(i, j) = 0;
                    }
                    isDegenerate = true;
                } else {
                    break;
                }
            }
            matP = matV.inv() * matV2;
        }
        
        if (isDegenerate)
        {
            cv::Mat matX2(6, 1, CV_32F, cv::Scalar::all(0));
            matX.copyTo(matX2);
            matX = matP * matX2; // here matX is xu', matX2 is xu
        }
        
        transformTobeMapped[0] += matX.at<float>(0, 0);
        transformTobeMapped[1] += matX.at<float>(1, 0);
        transformTobeMapped[2] += matX.at<float>(2, 0);
        transformTobeMapped[3] += matX.at<float>(3, 0);
        transformTobeMapped[4] += matX.at<float>(4, 0);
        transformTobeMapped[5] += matX.at<float>(5, 0);

        float deltaR = sqrt(
                            pow(pcl::rad2deg(matX.at<float>(0, 0)), 2) +
                            pow(pcl::rad2deg(matX.at<float>(1, 0)), 2) +
                            pow(pcl::rad2deg(matX.at<float>(2, 0)), 2));
        float deltaT = sqrt(
                            pow(matX.at<float>(3, 0) * 100, 2) +
                            pow(matX.at<float>(4, 0) * 100, 2) +
                            pow(matX.at<float>(5, 0) * 100, 2));

        if (deltaR < 0.05 && deltaT < 0.05) {
            return true; // converged
        }
        return false; // keep optimizing
    }

    void scan2MapOptimization()
    {
        if (keyPosesTarget3D->points.empty())
            return;
        // cout<<"corner, surf points: "<<lidarCloudCornerLastDSNum<<" "<<lidarCloudSurfLastDSNum<<endl;
        if (lidarCloudCornerLastDSNum > edgeFeatureMinValidNum && lidarCloudSurfLastDSNum > surfFeatureMinValidNum)
        {
            kdtreeCornerFromMap->setInputCloud(lidarCloudCornerFromMapDS);
            kdtreeSurfFromMap->setInputCloud(lidarCloudSurfFromMapDS);
            iterCount = 0;
            float cornerTime = 0, surfTime = 0;
            for (; iterCount < optIteration; iterCount++)
            {
                lidarCloudOri->clear();
                coeffSel->clear();
                TicToc corner;
                cornerOptimization(iterCount);
                cornerTime += corner.toc();
                TicToc surf;
                surfOptimization(iterCount);
                surfTime += surf.toc();
                combineOptimizationCoeffs();

                if (LMOptimization(iterCount) == true)
                {
                    // cout<<"converged"<<endl;
                    break;              
                }
            }
            *edgeErrorCloud += *tmpEdgeErrorCloud;
            *surfErrorCloud += *tmpSurfErrorCloud;
            // if (isDegenerate)             ROS_WARN("isDegenerate!");

            double regiError = accumulate(mapRegistrationError.begin(),mapRegistrationError.end(),0.0);
            double errorSize = mapRegistrationError.size();
            regiError /= errorSize;

            double inlier_ratio2 = (double)lidarCloudOri->size()/( lidarCloudSurfLastDSNum+ lidarCloudCornerLastDSNum); // same as mapRegistrationError.size()
            int inlierCnt = 0;
            // double regiError2 = 0;
            for (int i = 0; i < errorSize; i++)
            {
                // regiError2 += mapRegistrationError[i]*mapRegistrationError[i];
                if (mapRegistrationError[i] < inlierThreshold) inlierCnt++; // <0.1 means "properly matched"
            }
            // regiError2 = sqrt(regiError2/errorSize);
            double inlier_ratio = inlierCnt/ errorSize;

            vector<double> tmp;
            tmp.push_back((double)cloudInfoTime);
            tmp.push_back(regiError);
            tmp.push_back(inlier_ratio);
            tmp.push_back(edgePointCorrNum);
            tmp.push_back(surfPointCorrNum);
            if (mappingTimeVec.empty()) tmp.push_back(0);
            else tmp.push_back(mappingTimeVec.back()); // mapping time of the last frame
            tmp.push_back(cornerTime);
            tmp.push_back(surfTime);
            tmp.push_back(inlier_ratio2);
            // ROS_INFO_STREAM("error: "<<regiError <<" inlier ratio: "<<  inlier_ratio);
            mappingLogs.push_back( tmp );

            // ROS_INFO_STREAM(" mapping error: "<<regiError);
            if ( inlier_ratio < startTemporaryMappingInlierRatioThre)
            {
                poorlyMatched = true;
            } 
                
            if(poorlyMatched == true && localizationMode && temporaryMappingMode == false)
            {
            // important: TMM probably won't solve problem if the environment is degenerate!
            // partking lot needs tmm, so forfeit!
                // if (isDegenerate == false) // 20120429
                // {
                    ROS_INFO_STREAM("At time "<< cloudInfoTime - rosTimeStart <<" sec, Entering temporary mapping mode due to poor mapping performace");
                    ROS_INFO_STREAM("Inlier ratio: "<< inlier_ratio);                        
                    temporaryMappingMode = true; // here is the case for outdated map
                    startTemporaryMappingIndex = temporaryCloudKeyPoses3D->size();
                    frameTobeAbandoned = true;
                    for(int i=0;i<6;i++)  transformTobeMapped[i] = transformBeforeMapped[i];
                    TMMcount++;
                // }
                // else
                // {
                //     // ROS_INFO_STREAM("At time "<< cloudInfoTime - rosTimeStart <<" sec, avoid temporary mapping mode due to degeneracy");
                // }
            }

            if(poseGuessFromRvizAvailable) 
            {
                for(int i=0;i<6;i++)     transformTobeMapped[i] = transformBeforeMapped[i];
            }
                
        } 
        else 
        {
            ROS_WARN("Not enough features! Only %d edge and %d planar features available.", lidarCloudCornerLastDSNum, lidarCloudSurfLastDSNum);
        }

    }

    bool saveFrame()
    {
        if (keyPosesTarget3D->points.empty())    return true;
        if(poorlyMatched == true) 
        {
            // ROS_WARN("Poorly matched, keyframe dropped");
            return false; 
        }
        // allow overlapped area to display loop closures
        Eigen::Affine3f transStart;
        if (localizationMode)
        {
            if (temporaryCloudKeyPoses6D->points.empty() == true ) return true; 
            transStart = pclPointToAffine3f(temporaryCloudKeyPoses6D->back());  
        }            
        else
            transStart = pclPointToAffine3f(cloudKeyPoses6D->back());

        Eigen::Affine3f transFinal = pcl::getTransformation(transformTobeMapped[3], transformTobeMapped[4], transformTobeMapped[5], 
                                                            transformTobeMapped[0], transformTobeMapped[1], transformTobeMapped[2]);
        Eigen::Affine3f transBetween = transStart.inverse() * transFinal;
        float x, y, z, roll, pitch, yaw;
        pcl::getTranslationAndEulerAngles(transBetween, x, y, z, roll, pitch, yaw);

        float dist = sqrt(x*x + y*y + z*z);
        if (dist < surroundingkeyframeAddingDistThreshold) // for paranomic lidar angle threshold is not needed
            return false;
        // cout<<"dist: "<<dist<<endl;
        return true;
    }

    void addOdomFactor()
    {
        if (cloudKeyPoses3D->points.empty())
        {
            noiseModel::Diagonal::shared_ptr priorNoise = noiseModel::Diagonal::Variances((Vector(6) << 1e-2, 1e-2, M_PI*M_PI, 1e8, 1e8, 1e8).finished()); // rad*rad, meter*meter
            gtSAMgraph.add(PriorFactor<Pose3>(0, trans2gtsamPose(transformTobeMapped), priorNoise));
            initialEstimate.insert(0, trans2gtsamPose(transformTobeMapped));
        }
        else
        {
            noiseModel::Diagonal::shared_ptr odometryNoise = noiseModel::Diagonal::Variances((Vector(6) << 1e-6, 1e-6, 1e-6, 1e-4, 1e-4, 1e-4).finished());
            gtsam::Pose3 poseFrom = pclPointTogtsamPose3(cloudKeyPoses6D->points.back());
            gtsam::Pose3 poseTo   = trans2gtsamPose(transformTobeMapped);
            gtSAMgraph.add(BetweenFactor<Pose3>(cloudKeyPoses3D->size()-1, cloudKeyPoses3D->size(), poseFrom.between(poseTo), odometryNoise));
            initialEstimate.insert(cloudKeyPoses3D->size(), poseTo);
        }
    }


    void addGPSFactor()
    {
        if (gpsQueue.empty())
            return;

        // wait for system initialized and settles down
        if (cloudKeyPoses3D->points.empty())
            return;
        else
        {
            if (pointDistance(cloudKeyPoses3D->front(), cloudKeyPoses3D->back()) < 5.0)
                return;
        }

        if (poseCovariance(3,3) < poseCovThreshold && poseCovariance(4,4) < poseCovThreshold)
            return;

        // last gps position
        static PointType lastGPSPoint;

        while (!gpsQueue.empty())
        {
            if (gpsQueue.front().header.stamp.toSec() < cloudInfoTime - 0.2)
            {
                // cout<<"message too old"<<endl;
                gpsQueue.pop_front();
            }
            else if (gpsQueue.front().header.stamp.toSec() > cloudInfoTime + 0.2)
            {
                // cout<<" message too new" << endl;
                break;
            }
            else
            {
                nav_msgs::Odometry thisGPS = gpsQueue.front();
                gpsQueue.pop_front();
                // ground truth
                float noise_x = thisGPS.pose.covariance[0];
                float noise_y = thisGPS.pose.covariance[7];
                float noise_z = thisGPS.pose.covariance[14];
                if (noise_x > gpsCovThreshold || noise_y > gpsCovThreshold)
                    continue;

                float gps_x = thisGPS.pose.pose.position.x;
                float gps_y = thisGPS.pose.pose.position.y;
                float gps_z = thisGPS.pose.pose.position.z;
                if (!useGpsElevation)
                {
                    gps_z = transformTobeMapped[5];
                    noise_z = 0.01;
                }

                if (abs(gps_x) < 1e-6 && abs(gps_y) < 1e-6)
                    continue;

                // Add GPS every a few meters
                PointType curGPSPoint;
                curGPSPoint.x = gps_x;
                curGPSPoint.y = gps_y;
                curGPSPoint.z = gps_z ; // for body-lidar coor. trans.
                if (pointDistance(curGPSPoint, lastGPSPoint) < 5.0)
                    continue;
                else
                    lastGPSPoint = curGPSPoint;

                gtsam::Vector Vector3(3);
                Vector3 << max(noise_x, 1.0f), max(noise_y, 1.0f), max(noise_z, 1.0f); 
                noiseModel::Diagonal::shared_ptr gps_noise = noiseModel::Diagonal::Variances(Vector3);
                gtsam::GPSFactor gps_factor(cloudKeyPoses3D->size(), gtsam::Point3(gps_x, gps_y, gps_z), gps_noise);
                gtSAMgraph.add(gps_factor);
                aLoopIsClosed = true;
                ROS_INFO("add gps factor...");
                break;
            }
        }
    }

    void addLoopFactor()
    {
        if (loopIndexQueue.empty())
            return;

        for (int i = 0; i < (int)loopIndexQueue.size(); ++i)
        {
            int indexFrom = loopIndexQueue[i].first;
            int indexTo = loopIndexQueue[i].second;
            gtsam::Pose3 poseBetween = loopPoseQueue[i];
            // gtsam::noiseModel::Diagonal::shared_ptr noiseBetween = loopNoiseQueue[i];
            auto noiseBetween = loopNoiseQueue[i];
            gtSAMgraph.add(BetweenFactor<Pose3>(indexFrom, indexTo, poseBetween, noiseBetween));
        }

        loopIndexQueue.clear();
        loopPoseQueue.clear();
        loopNoiseQueue.clear();
        aLoopIsClosed = true;
    }

    void saveTemporaryKeyframes()
    {
        //save temporary key poses
        if (saveFrame() == false) return;

        int temporaryKeyPoseSize = temporaryCloudKeyPoses3D->size();
        PointType thisPose3D;
        PointTypePose thisPose6D;
        thisPose3D.x = transformTobeMapped[3];
        thisPose3D.y = transformTobeMapped[4];
        thisPose3D.z = transformTobeMapped[5];
        thisPose3D.intensity = temporaryKeyPoseSize; // this can be used as keyframe index

        thisPose6D.x = thisPose3D.x;
        thisPose6D.y = thisPose3D.y;
        thisPose6D.z = thisPose3D.z;
        thisPose6D.intensity = thisPose3D.intensity ; // this can be used as index
        thisPose6D.roll  = transformTobeMapped[0];
        thisPose6D.pitch = transformTobeMapped[1];
        thisPose6D.yaw   = transformTobeMapped[2];
        thisPose6D.time = cloudInfoTime;

        temporaryCloudKeyPoses3D->push_back(thisPose3D);
        temporaryCloudKeyPoses6D->push_back(thisPose6D);
        // save all the received edge and surf points
        pcl::PointCloud<PointType>::Ptr thisCornerKeyFrame(new pcl::PointCloud<PointType>());
        pcl::PointCloud<PointType>::Ptr thisSurfKeyFrame(new pcl::PointCloud<PointType>());
        pcl::copyPointCloud(*lidarCloudCornerLast,  *thisCornerKeyFrame);
        pcl::copyPointCloud(*lidarCloudSurfLast,    *thisSurfKeyFrame);
        // save key frame cloud
        temporaryCornerCloudKeyFrames.push_back(thisCornerKeyFrame); // 这个全局都存着，但每次局部匹配只搜索50m内的关键帧
        temporarySurfCloudKeyFrames.push_back(thisSurfKeyFrame);

        if (!temporaryMappingMode && temporaryKeyPoseSize > slidingWindowSize) // sliding-window local map
        {
            temporaryCloudKeyPoses3D->erase(temporaryCloudKeyPoses3D->begin());
            temporaryCloudKeyPoses6D->erase(temporaryCloudKeyPoses6D->begin());
            temporaryCornerCloudKeyFrames.erase(temporaryCornerCloudKeyFrames.begin());
            temporarySurfCloudKeyFrames.erase(temporarySurfCloudKeyFrames.begin());
            // reindexing: key poses and key frames are corresponding with respect to the adding sequence
            for (int i = 0 ; i< (int)temporaryCloudKeyPoses3D->size(); i++)
            {
                temporaryCloudKeyPoses3D->points[i].intensity = i;
                temporaryCloudKeyPoses6D->points[i].intensity = i;
            }
        }
        //publish temporary keyposes for visualization
        publishCloud(&pubKeyPosesTmp,temporaryCloudKeyPoses3D,timeLidarInfoStamp, mapFrame);
    }

    void saveKeyFramesAndFactor()
    {
        if (saveFrame() == false)
            return;

        // odom factor
        addOdomFactor();

        // gps factor
        addGPSFactor();

        // loop factor
        addLoopFactor();

        // cout << "****************************************************" << endl;
        // gtSAMgraph.print("GTSAM Graph:\n");

        // update iSAM
        isam->update(gtSAMgraph, initialEstimate);
        isam->update();
        if (aLoopIsClosed == true)
        {
            isam->update();
            isam->update();
            isam->update();
            isam->update();
            isam->update();
        }
        gtSAMgraph.resize(0);
        initialEstimate.clear();

        Pose3 latestEstimate;
        isamCurrentEstimate = isam->calculateEstimate();
        latestEstimate = isamCurrentEstimate.at<Pose3>(isamCurrentEstimate.size()-1);
        // cout << "****************************************************" << endl;
        // isamCurrentEstimate.print("gtsam current estimate: ");

        //save key poses
        PointType thisPose3D;
        PointTypePose thisPose6D;
        thisPose3D.x = latestEstimate.translation().x();
        thisPose3D.y = latestEstimate.translation().y();
        thisPose3D.z = latestEstimate.translation().z();
        thisPose3D.intensity = cloudKeyPoses3D->size(); // this can be used as keyframe index
        cloudKeyPoses3D->push_back(thisPose3D);

        thisPose6D.x = thisPose3D.x;
        thisPose6D.y = thisPose3D.y;
        thisPose6D.z = thisPose3D.z;
        thisPose6D.intensity = thisPose3D.intensity ; // this can be used as index
        thisPose6D.roll  = latestEstimate.rotation().roll();
        thisPose6D.pitch = latestEstimate.rotation().pitch();
        thisPose6D.yaw   = latestEstimate.rotation().yaw();
        thisPose6D.time = cloudInfoTime;
        cloudKeyPoses6D->push_back(thisPose6D);

        // cout << "****************************************************" << endl;
        // cout << "Pose covariance:" << isam->marginalCovariance(isamCurrentEstimate.size()-1) << endl;
        poseCovariance = isam->marginalCovariance(isamCurrentEstimate.size()-1);
        // cout<<"x cov: "<< poseCovariance(0,0)<< " y cov: "<<poseCovariance(1,1)<<endl;

        // cout<<"Before opt: "<<transformTobeMapped[3]<<endl;;
        // save updated transform
        transformTobeMapped[0] = latestEstimate.rotation().roll();
        transformTobeMapped[1] = latestEstimate.rotation().pitch();
        transformTobeMapped[2] = latestEstimate.rotation().yaw();
        transformTobeMapped[3] = latestEstimate.translation().x();
        transformTobeMapped[4] = latestEstimate.translation().y();
        transformTobeMapped[5] = latestEstimate.translation().z();
        // cout<<"After opt: "<< transformTobeMapped[3]<<endl;
        // save all the received edge and surf points
        pcl::PointCloud<PointType>::Ptr thisCornerKeyFrame(new pcl::PointCloud<PointType>());
        pcl::PointCloud<PointType>::Ptr thisSurfKeyFrame(new pcl::PointCloud<PointType>());       
        // if pushing the original, it changes in the vec along with lidarCloudCornerLast
        pcl::copyPointCloud(*lidarCloudCornerLast,  *thisCornerKeyFrame);
        pcl::copyPointCloud(*lidarCloudSurfLast,    *thisSurfKeyFrame); 
        cornerCloudKeyFrames.push_back(thisCornerKeyFrame); 
        surfCloudKeyFrames.push_back(thisSurfKeyFrame);

        // save path for visualization
        updatePath(thisPose6D);
        
        // if(saveRawCloud) 
        //     pcl::io::savePCDFileBinary(saveMapDirectory + "/"+ to_string(cloudKeyPoses3D->size()-1) + ".pcd", *lidarCloudRaw);
        



    }

    void correctPoses()
    {
        if (cloudKeyPoses3D->points.empty())
            return;

        if (aLoopIsClosed == true)
        {
            // clear map cache
            lidarCloudMapContainer.clear();
            // clear path
            globalPath.poses.clear();
            // update key poses
            int numPoses = isamCurrentEstimate.size();
            for (int i = 0; i < numPoses; ++i)
            {
                cloudKeyPoses3D->points[i].x = isamCurrentEstimate.at<Pose3>(i).translation().x();
                cloudKeyPoses3D->points[i].y = isamCurrentEstimate.at<Pose3>(i).translation().y();
                cloudKeyPoses3D->points[i].z = isamCurrentEstimate.at<Pose3>(i).translation().z();

                cloudKeyPoses6D->points[i].x = cloudKeyPoses3D->points[i].x;
                cloudKeyPoses6D->points[i].y = cloudKeyPoses3D->points[i].y;
                cloudKeyPoses6D->points[i].z = cloudKeyPoses3D->points[i].z;
                cloudKeyPoses6D->points[i].roll  = isamCurrentEstimate.at<Pose3>(i).rotation().roll();
                cloudKeyPoses6D->points[i].pitch = isamCurrentEstimate.at<Pose3>(i).rotation().pitch();
                cloudKeyPoses6D->points[i].yaw   = isamCurrentEstimate.at<Pose3>(i).rotation().yaw();
                updatePath(cloudKeyPoses6D->points[i]);
            }

            aLoopIsClosed = false;
        }
    }

    void updatePath(const PointTypePose& pose_in)
    {
        geometry_msgs::PoseStamped pose_stamped;
        pose_stamped.header.stamp = ros::Time().fromSec(pose_in.time);
        pose_stamped.header.frame_id = mapFrame;
        pose_stamped.pose.position.x = pose_in.x;
        pose_stamped.pose.position.y = pose_in.y;
        pose_stamped.pose.position.z = pose_in.z;
        tf::Quaternion q = tf::createQuaternionFromRPY(pose_in.roll, pose_in.pitch, pose_in.yaw);
        pose_stamped.pose.orientation.x = q.x();
        pose_stamped.pose.orientation.y = q.y();
        pose_stamped.pose.orientation.z = q.z();
        pose_stamped.pose.orientation.w = q.w();
        globalPath.poses.push_back(pose_stamped);
    }

    void publishOdometry()
    {
        // Publish odometry for ROS (global)
        nav_msgs::Odometry lidarOdometryROS;
        lidarOdometryROS.header.stamp = timeLidarInfoStamp;
        lidarOdometryROS.header.frame_id = mapFrame;
        lidarOdometryROS.child_frame_id = odometryFrame;
        lidarOdometryROS.pose.pose.position.x = transformTobeMapped[3];
        lidarOdometryROS.pose.pose.position.y = transformTobeMapped[4];
        lidarOdometryROS.pose.pose.position.z = transformTobeMapped[5];
        // cout<<transformTobeMapped[3]<<" "<<transformTobeMapped[4]<<" "<<endl;
        lidarOdometryROS.pose.pose.orientation = tf::createQuaternionMsgFromRollPitchYaw(transformTobeMapped[0], transformTobeMapped[1], transformTobeMapped[2]);
        pubLidarOdometryGlobal.publish(lidarOdometryROS);
        globalOdometry.push_back(lidarOdometryROS);
    }

    void publishFrames()
    {
        // Publish surrounding key frames: 
        // for reloc, it is not recent, it is just surrouding 
        // for temporary mapping, it should be all temporary clouds
        pcl::PointCloud<PointType>::Ptr cloudLocal(new pcl::PointCloud<PointType>());
        *cloudLocal += *lidarCloudSurfFromMap;
        *cloudLocal += *lidarCloudCornerFromMap;
        publishCloud(&pubRecentKeyFrames, cloudLocal, timeLidarInfoStamp, mapFrame); 

        globalPath.header.stamp = timeLidarInfoStamp;
        globalPath.header.frame_id = mapFrame;
        pubPath.publish(globalPath);
    }

    // ~mapOptimization(){
    // }
};


int main(int argc, char** argv)
{
    ros::init(argc, argv, "kloam");

    mapOptimization MO;

    ROS_INFO("\033[1;32m----> Map Optimization Started.\033[0m");
    
    std::thread loopthread(&mapOptimization::loopClosureThread, &MO);
    std::thread visualizeMapThread(&mapOptimization::visualizeGlobalMapThread, &MO);
    std::thread mappingThread{&mapOptimization::run,&MO};
    ros::spin();

    loopthread.join();
    visualizeMapThread.join();
    mappingThread.join();
    
    return 0;
}