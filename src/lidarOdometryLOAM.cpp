// Copyright 2013, Ji Zhang, Carnegie Mellon University
// Further contributions copyright (c) 2016, Southwest Research Institute
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from this
//    software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// This is an implementation of the algorithm described in the following papers:
//   J. Zhang and S. Singh. LOAM: Lidar Odometry and Mapping in Real-time.
//     Robotics: Science and Systems Conference (RSS). Berkeley, CA, July 2014.
//   T. Shan and B. Englot. LeGO-LOAM: Lightweight and Ground-Optimized Lidar Odometry and Mapping on Variable Terrain
//      IEEE/RSJ International Conference on Intelligent Robots and Systems (IROS). October 2018.

#include "utility.h"
#include "kloam/cloud_info.h"
class lidarOdometry : public ParamServer{

private:
    bool deskewEnabled = false;

    float odomOptIteration = 25;
    int odomEdgeFeatureMinValidNum = 10;
    int odomSurfFeatureMinValidNum = 50;

    float cornerOutlierCoeff = 1.8;
    float surfOutlierCoeff = 1.8;
    float deltaRthre = 0.1;
    float deltaTthre = 0.1;
    nav_msgs::Path odometryPath;

    kloam::cloud_info cloudInfo;
    std::mutex mtx;
    pcl::VoxelGrid<PointType> downSizeFilterCorner;
    pcl::VoxelGrid<PointType> downSizeFilterSurf;

    std::vector<PointType> laserCloudOriCornerVec; // corner point holder for parallel computation
    std::vector<PointType> coeffSelCornerVec;
    std::vector<bool> laserCloudOriCornerFlag;
    std::vector<PointType> laserCloudOriSurfVec; // surf point holder for parallel computation
    std::vector<PointType> coeffSelSurfVec;
    std::vector<bool> laserCloudOriSurfFlag;

    Eigen::Affine3f transCur;

	ros::NodeHandle nh;
    ros::Subscriber subCloudInfo;

    std_msgs::Header cloudHeader;

    ros::Publisher pubLidarOdometry;
    ros::Publisher pubLidarPath;
    ros::Publisher pubFeatureCloud;
    ros::Publisher pubCloudInfo;
    ros::Publisher pubFeatureCloudCur;
    ros::Publisher pubFeatureCloudLast;
    bool systemInitedLM;
    // notice here transformCur is from T_k_k+1, different from LeGO-LOAM/LOAM
    // it maps points from T_k+1 to T_k, similar to mapOptimization module
    float transformCur[6]; 
    float transformSum[6];

    pcl::PointCloud<PointType>::Ptr laserCloudCornerLast;
    pcl::PointCloud<PointType>::Ptr laserCloudSurfLast;
    // pcl::PointCloud<PointType>::Ptr laserCloudCornerLastDS;
    // pcl::PointCloud<PointType>::Ptr laserCloudSurfLast;
    pcl::PointCloud<PointType>::Ptr laserCloudCornerCur;
    pcl::PointCloud<PointType>::Ptr laserCloudSurfCur;
    pcl::PointCloud<PointType>::Ptr laserCloudCornerCurDS;
    pcl::PointCloud<PointType>::Ptr laserCloudSurfCurDS;
    int laserCloudCornerCurDSNum;
    int laserCloudSurfCurDSNum;

    vector<int> _pointSearchCornerInd1;
    vector<int> _pointSearchCornerInd2;
    vector<int> _pointSearchSurfInd1;
    vector<int> _pointSearchSurfInd2;
    vector<int> _pointSearchSurfInd3;
    pcl::PointCloud<PointType>::Ptr laserCloudOri;
    pcl::PointCloud<PointType>::Ptr coeffSel;

    pcl::KdTreeFLANN<PointType>::Ptr kdtreeCornerLast;
    pcl::KdTreeFLANN<PointType>::Ptr kdtreeSurfLast;

    nav_msgs::Odometry laserOdometry;

    tf::TransformBroadcaster tfBroadcaster;
    tf::StampedTransform laserOdometryTrans;

    bool isDegenerate;
    cv::Mat matP;

public:
    vector<float> odometryTime;
    lidarOdometry()
    {
        subCloudInfo = nh.subscribe<kloam::cloud_info>("/kloam/feature/cloud_info", 1, &lidarOdometry::laserCloudInfoHandler, this, ros::TransportHints().tcpNoDelay());
        pubLidarOdometry = nh.advertise<nav_msgs::Odometry> ("/kloam/lidarOdometry/laser_odom_to_init", 1);      // how to visualize in rviz?
        pubCloudInfo = nh.advertise<kloam::cloud_info> ("/kloam/lidarOdometry/cloud_info_with_guess", 1); 
        pubLidarPath = nh.advertise<nav_msgs::Path> ("/kloam/lidarOdometry/laser_odom_path", 1); 
        pubFeatureCloudCur = nh.advertise<sensor_msgs::PointCloud2>("/kloam/lidarOdometry/feature_cloud_cur", 1);    
        pubFeatureCloudLast = nh.advertise<sensor_msgs::PointCloud2>("/kloam/lidarOdometry/feature_cloud_last", 1);
        initializationValue();
    }

    void initializationValue()
    {
        transCur = Eigen::Affine3f::Identity();
        odometryPath.poses.clear();


        laserCloudOri.reset(new pcl::PointCloud<PointType>());
        coeffSel.reset(new pcl::PointCloud<PointType>());

        laserCloudOriCornerVec.resize(N_SCAN * Horizon_SCAN);
        coeffSelCornerVec.resize(N_SCAN * Horizon_SCAN);
        laserCloudOriCornerFlag.resize(N_SCAN * Horizon_SCAN);
        laserCloudOriSurfVec.resize(N_SCAN * Horizon_SCAN);
        coeffSelSurfVec.resize(N_SCAN * Horizon_SCAN);
        laserCloudOriSurfFlag.resize(N_SCAN * Horizon_SCAN);

        std::fill(laserCloudOriCornerFlag.begin(), laserCloudOriCornerFlag.end(), false);
        std::fill(laserCloudOriSurfFlag.begin(), laserCloudOriSurfFlag.end(), false);
        
        for (int i = 0; i < 6; ++i){
            transformCur[i] = 0;
            transformSum[i] = 0;
        }

        systemInitedLM = false;

        laserCloudCornerLast.reset(new pcl::PointCloud<PointType>());
        laserCloudSurfLast.reset(new pcl::PointCloud<PointType>());
        laserCloudCornerCur.reset(new pcl::PointCloud<PointType>());
        laserCloudSurfCur.reset(new pcl::PointCloud<PointType>());
        laserCloudCornerCurDS.reset(new pcl::PointCloud<PointType>());
        laserCloudSurfCurDS.reset(new pcl::PointCloud<PointType>());

        laserCloudOri.reset(new pcl::PointCloud<PointType>());
        coeffSel.reset(new pcl::PointCloud<PointType>());

        kdtreeCornerLast.reset(new pcl::KdTreeFLANN<PointType>());
        kdtreeSurfLast.reset(new pcl::KdTreeFLANN<PointType>());

        laserOdometry.header.frame_id =  mapFrame;
        laserOdometry.child_frame_id = lidarFrame;

        laserOdometryTrans.frame_id_ =  mapFrame;
        laserOdometryTrans.child_frame_id_ = lidarFrame;
        
        isDegenerate = false;
        matP = cv::Mat(6, 6, CV_32F, cv::Scalar::all(0));
    }

     // ZXY欧拉角
     // YXZ rotation sequence, body-fixed ref frame, right-multiply
    void TransformToStart(PointType const * const pi, PointType * const po)
    {
        float s = 1.0;
        if (deskewEnabled)
            s = 10 * (pi->intensity - int(pi->intensity));
        
        float rx = s * transformCur[0];
        float ry = s * transformCur[1];
        float rz = s * transformCur[2];
        float tx = s * transformCur[3];
        float ty = s * transformCur[4];
        float tz = s * transformCur[5];

        float x1 = cos(rz) * (pi->x - tx) + sin(rz) * (pi->y - ty);
        float y1 = -sin(rz) * (pi->x - tx) + cos(rz) * (pi->y - ty);
        float z1 = (pi->z - tz);

        float x2 = x1;
        float y2 = cos(rx) * y1 + sin(rx) * z1;
        float z2 = -sin(rx) * y1 + cos(rx) * z1;

        po->x = cos(ry) * x2 - sin(ry) * z2;
        po->y = y2;
        po->z = sin(ry) * x2 + cos(ry) * z2;
        po->intensity = pi->intensity;
    }

void TransformToEnd(PointType const * const pi, PointType * const po)
    {
        //ZXY
        float s = 1.0;
        if (deskewEnabled)
            s = 10 * (pi->intensity - int(pi->intensity));
        else return;

        float rx = s * transformCur[0];
        float ry = s * transformCur[1];
        float rz = s * transformCur[2];
        float tx = s * transformCur[3];
        float ty = s * transformCur[4];
        float tz = s * transformCur[5];

        float x1 = cos(rz) * (pi->x - tx) + sin(rz) * (pi->y - ty);
        float y1 = -sin(rz) * (pi->x - tx) + cos(rz) * (pi->y - ty);
        float z1 = (pi->z - tz);

        float x2 = x1;
        float y2 = cos(rx) * y1 + sin(rx) * z1;
        float z2 = -sin(rx) * y1 + cos(rx) * z1;

        float x3 = cos(ry) * x2 - sin(ry) * z2;
        float y3 = y2;
        float z3 = sin(ry) * x2 + cos(ry) * z2;

        rx = transformCur[0];
        ry = transformCur[1];
        rz = transformCur[2];
        tx = transformCur[3];
        ty = transformCur[4];
        tz = transformCur[5];

        float x4 = cos(ry) * x3 + sin(ry) * z3;
        float y4 = y3;
        float z4 = -sin(ry) * x3 + cos(ry) * z3;

        float x5 = x4;
        float y5 = cos(rx) * y4 - sin(rx) * z4;
        float z5 = sin(rx) * y4 + cos(rx) * z4;

        float x6 = cos(rz) * x5 - sin(rz) * y5 + tx;
        float y6 = sin(rz) * x5 + cos(rz) * y5 + ty;
        float z6 = z5 + tz;

        po->x = x6;
        po->y = y6;
        po->z = z6;
        po->intensity = int(pi->intensity);
    }

    void AccumulateRotation(float cx, float cy, float cz, float lx, float ly, float lz, 
                            float &ox, float &oy, float &oz)
    {
        float srx = cos(lx)*cos(cx)*sin(ly)*sin(cz) - cos(cx)*cos(cz)*sin(lx) - cos(lx)*cos(ly)*sin(cx);
        ox = -asin(srx);

        float srycrx = sin(lx)*(cos(cy)*sin(cz) - cos(cz)*sin(cx)*sin(cy)) + cos(lx)*sin(ly)*(cos(cy)*cos(cz) 
                     + sin(cx)*sin(cy)*sin(cz)) + cos(lx)*cos(ly)*cos(cx)*sin(cy);
        float crycrx = cos(lx)*cos(ly)*cos(cx)*cos(cy) - cos(lx)*sin(ly)*(cos(cz)*sin(cy) 
                     - cos(cy)*sin(cx)*sin(cz)) - sin(lx)*(sin(cy)*sin(cz) + cos(cy)*cos(cz)*sin(cx));
        oy = atan2(srycrx / cos(ox), crycrx / cos(ox));

        float srzcrx = sin(cx)*(cos(lz)*sin(ly) - cos(ly)*sin(lx)*sin(lz)) + cos(cx)*sin(cz)*(cos(ly)*cos(lz) 
                     + sin(lx)*sin(ly)*sin(lz)) + cos(lx)*cos(cx)*cos(cz)*sin(lz);
        float crzcrx = cos(lx)*cos(lz)*cos(cx)*cos(cz) - cos(cx)*sin(cz)*(cos(ly)*sin(lz) 
                     - cos(lz)*sin(lx)*sin(ly)) - sin(cx)*(sin(ly)*sin(lz) + cos(ly)*cos(lz)*sin(lx));
        oz = atan2(srzcrx / cos(ox), crzcrx / cos(ox));
    }

    void integrateTransformation(){
        float rx, ry, rz, tx, ty, tz;
        AccumulateRotation(transformSum[0], transformSum[1], transformSum[2], 
                           -transformCur[0], -transformCur[1], -transformCur[2], rx, ry, rz); 

        float x1 = cos(rz) * transformCur[3] 
                 - sin(rz) * transformCur[4];
        float y1 = sin(rz) * transformCur[3] 
                 + cos(rz) * transformCur[4];
        float z1 = transformCur[5];

        float x2 = x1;
        float y2 = cos(rx) * y1 - sin(rx) * z1;
        float z2 = sin(rx) * y1 + cos(rx) * z1;

        tx = transformSum[3] - (cos(ry) * x2 + sin(ry) * z2);
        ty = transformSum[4] - y2;
        tz = transformSum[5] - (-sin(ry) * x2 + cos(ry) * z2);

        transformSum[0] = rx;
        transformSum[1] = ry;
        transformSum[2] = rz;
        transformSum[3] = tx;
        transformSum[4] = ty;
        transformSum[5] = tz;
    }

    double rad2deg(double radians)
    {
        return radians * 180.0 / M_PI;
    }

    double deg2rad(double degrees)
    {
        return degrees * M_PI / 180.0;
    }
    
    void cornerOptimization(int iterCount)
    {
        vector<int> pointSearchInd;
        vector<float> pointSearchSqDis;
        PointType pointSel, coeff, tripod1, tripod2;
        for (int i = 0; i < laserCloudCornerCurDSNum; i++)
        {
            TransformToStart(&laserCloudCornerCurDS->points[i], &pointSel);
            if (iterCount % 5 == 0)
            {
               kdtreeCornerLast->nearestKSearch(pointSel, 1, pointSearchInd, pointSearchSqDis);
               int closestPointInd = -1, minPointInd2 = -1;
               if (pointSearchSqDis[0] < 25) // as in loam, wtf?
               {
                  closestPointInd = pointSearchInd[0];
                  int closestPointScan = int(laserCloudCornerLast->points[closestPointInd].intensity);

                  float pointSqDis, minPointSqDis2 = 25;
                  for (int j = closestPointInd + 1; j < laserCloudCornerCurDSNum; j++)
                  {
                     if (int(laserCloudCornerLast->points[j].intensity) > closestPointScan + 2.5*downsampleRate)
                     {
                        break;
                     }

                     pointSqDis = pointDistance(laserCloudCornerLast->points[j], pointSel);

                     if (int(laserCloudCornerLast->points[j].intensity) > closestPointScan)
                     {
                        if (pointSqDis < minPointSqDis2)
                        {
                           minPointSqDis2 = pointSqDis;
                           minPointInd2 = j;
                        }
                     }
                  }
                  for (int j = closestPointInd - 1; j >= 0; j--)
                  {
                     if (int(laserCloudCornerLast->points[j].intensity) < closestPointScan - 2.5*downsampleRate)
                     {
                        break;
                     }

                     pointSqDis = pointDistance(laserCloudCornerLast->points[j], pointSel);

                     if (int(laserCloudCornerLast->points[j].intensity) < closestPointScan)
                     {
                        if (pointSqDis < minPointSqDis2)
                        {
                           minPointSqDis2 = pointSqDis;
                           minPointInd2 = j;
                        }
                     }
                  }
               }

               _pointSearchCornerInd1[i] = closestPointInd;
               _pointSearchCornerInd2[i] = minPointInd2;
            }
            // here it should be >?
            if (_pointSearchCornerInd2[i] >= 0) // resize default to zero
            {
               tripod1 = laserCloudCornerLast->points[_pointSearchCornerInd1[i]];
               tripod2 = laserCloudCornerLast->points[_pointSearchCornerInd2[i]];

               float x0 = pointSel.x;
               float y0 = pointSel.y;
               float z0 = pointSel.z;
               float x1 = tripod1.x;
               float y1 = tripod1.y;
               float z1 = tripod1.z;
               float x2 = tripod2.x;
               float y2 = tripod2.y;
               float z2 = tripod2.z;

               float a012 = sqrt(((x0 - x1)*(y0 - y2) - (x0 - x2)*(y0 - y1))
                                 * ((x0 - x1)*(y0 - y2) - (x0 - x2)*(y0 - y1))
                                 + ((x0 - x1)*(z0 - z2) - (x0 - x2)*(z0 - z1))
                                 * ((x0 - x1)*(z0 - z2) - (x0 - x2)*(z0 - z1))
                                 + ((y0 - y1)*(z0 - z2) - (y0 - y2)*(z0 - z1))
                                 * ((y0 - y1)*(z0 - z2) - (y0 - y2)*(z0 - z1)));

               float l12 = sqrt((x1 - x2)*(x1 - x2) + (y1 - y2)*(y1 - y2) + (z1 - z2)*(z1 - z2));

               float la = ((y1 - y2)*((x0 - x1)*(y0 - y2) - (x0 - x2)*(y0 - y1))
                           + (z1 - z2)*((x0 - x1)*(z0 - z2) - (x0 - x2)*(z0 - z1))) / a012 / l12;

               float lb = -((x1 - x2)*((x0 - x1)*(y0 - y2) - (x0 - x2)*(y0 - y1))
                            - (z1 - z2)*((y0 - y1)*(z0 - z2) - (y0 - y2)*(z0 - z1))) / a012 / l12;

               float lc = -((x1 - x2)*((x0 - x1)*(z0 - z2) - (x0 - x2)*(z0 - z1))
                            + (y1 - y2)*((y0 - y1)*(z0 - z2) - (y0 - y2)*(z0 - z1))) / a012 / l12;

               float ld2 = a012 / l12; // Eq. (2)

               float s = 1;
               if (iterCount >= 5) // this matters!!!
               {
                  s = 1 - cornerOutlierCoeff * fabs(ld2);
               }

               coeff.x = s * la;
               coeff.y = s * lb;
               coeff.z = s * lc;
               coeff.intensity = s * ld2;

               if (s > 0.1 && ld2 != 0)
               {
                    laserCloudOriCornerVec[i] = laserCloudCornerCurDS->points[i];
                    coeffSelCornerVec[i] = coeff;
                    laserCloudOriCornerFlag[i] = true;
               }
            }
        }
    }

    void surfOptimization(int iterCount)
    {
        PointType pointSel, coeff, tripod1,tripod2, tripod3;
        vector<int> pointSearchInd;
        vector<float> pointSearchSqDis;
        for (int i = 0; i < laserCloudSurfCurDSNum; i++)
        {
            TransformToStart(&laserCloudSurfCurDS->points[i], &pointSel);
            if (iterCount % 5 == 0)
            {
               kdtreeSurfLast->nearestKSearch(pointSel, 1, pointSearchInd, pointSearchSqDis);
               int closestPointInd = -1, minPointInd2 = -1, minPointInd3 = -1;
               if (pointSearchSqDis[0] < 25)
               {
                  closestPointInd = pointSearchInd[0];
                  int closestPointScan = int(laserCloudSurfLast->points[closestPointInd].intensity);

                  float pointSqDis, minPointSqDis2 = 25, minPointSqDis3 = 25;
                  for (int j = closestPointInd + 1; j < laserCloudSurfCurDSNum; j++)
                  {
                     if (int(laserCloudSurfLast->points[j].intensity) > closestPointScan + 2.5*downsampleRate)
                     {
                        break;
                     }

                     pointSqDis = pointDistance(laserCloudSurfLast->points[j], pointSel);

                     if (int(laserCloudSurfLast->points[j].intensity) <= closestPointScan)
                     {
                        if (pointSqDis < minPointSqDis2)
                        {
                           minPointSqDis2 = pointSqDis;
                           minPointInd2 = j;
                        }
                     }
                     else
                     {
                        if (pointSqDis < minPointSqDis3)
                        {
                           minPointSqDis3 = pointSqDis;
                           minPointInd3 = j;
                        }
                     }
                  }
                  for (int j = closestPointInd - 1; j >= 0; j--)
                  {
                     if (int(laserCloudSurfLast->points[j].intensity) < closestPointScan - 2.5*downsampleRate)
                     {
                        break;
                     }

                     pointSqDis = pointDistance(laserCloudSurfLast->points[j], pointSel);

                     if (int(laserCloudSurfLast->points[j].intensity) >= closestPointScan)
                     {
                        if (pointSqDis < minPointSqDis2)
                        {
                           minPointSqDis2 = pointSqDis;
                           minPointInd2 = j;
                        }
                     }
                     else
                     {
                        if (pointSqDis < minPointSqDis3)
                        {
                           minPointSqDis3 = pointSqDis;
                           minPointInd3 = j;
                        }
                     }
                  }
               }

               _pointSearchSurfInd1[i] = closestPointInd;
               _pointSearchSurfInd2[i] = minPointInd2;
               _pointSearchSurfInd3[i] = minPointInd3;
            }

            if (_pointSearchSurfInd2[i] >=0 && _pointSearchSurfInd3[i] >= 0)
            {
               tripod1 = laserCloudSurfLast->points[_pointSearchSurfInd1[i]];
               tripod2 = laserCloudSurfLast->points[_pointSearchSurfInd2[i]];
               tripod3 = laserCloudSurfLast->points[_pointSearchSurfInd3[i]];

               float pa = (tripod2.y - tripod1.y) * (tripod3.z - tripod1.z)
                  - (tripod3.y - tripod1.y) * (tripod2.z - tripod1.z);
               float pb = (tripod2.z - tripod1.z) * (tripod3.x - tripod1.x)
                  - (tripod3.z - tripod1.z) * (tripod2.x - tripod1.x);
               float pc = (tripod2.x - tripod1.x) * (tripod3.y - tripod1.y)
                  - (tripod3.x - tripod1.x) * (tripod2.y - tripod1.y);
               float pd = -(pa * tripod1.x + pb * tripod1.y + pc * tripod1.z);

               float ps = sqrt(pa * pa + pb * pb + pc * pc);
               pa /= ps;
               pb /= ps;
               pc /= ps;
               pd /= ps;
               float pd2 = pa * pointSel.x + pb * pointSel.y + pc * pointSel.z + pd; //Eq. (3)
               float s = 1;
               if (iterCount >= 5)
               {
                  s = 1 - surfOutlierCoeff * fabs(pd2) / sqrt(pointDistance(pointSel));
               }
               coeff.x = s * pa;
               coeff.y = s * pb;
               coeff.z = s * pc;
               coeff.intensity = s * pd2;

               if (s > 0.1 && pd2 != 0)
               {
                    laserCloudOriSurfVec[i] = laserCloudSurfCurDS->points[i];
                    coeffSelSurfVec[i] = coeff;
                    laserCloudOriSurfFlag[i] = true;
               }
            }
         }
    }

    void combineOptimizationCoeffs()
    {
        // combine corner coeffs
        for (int i = 0; i < laserCloudCornerCurDSNum; ++i){
            if (laserCloudOriCornerFlag[i] == true){
                laserCloudOri->push_back(laserCloudOriCornerVec[i]);
                coeffSel->push_back(coeffSelCornerVec[i]);
            }
        }
        // combine surf coeffs
        for (int i = 0; i < laserCloudSurfCurDSNum; ++i){
            if (laserCloudOriSurfFlag[i] == true){
                laserCloudOri->push_back(laserCloudOriSurfVec[i]);
                coeffSel->push_back(coeffSelSurfVec[i]);
            }
        }
        // reset flag for next iteration
        std::fill(laserCloudOriCornerFlag.begin(), laserCloudOriCornerFlag.end(), false);
        std::fill(laserCloudOriSurfFlag.begin(), laserCloudOriSurfFlag.end(), false);
    }

    bool LMOptimization(int iterCount)
    {        
        TicToc opt;
        float s = 1;

        float srx = sin(s * transformCur[0]);
        float crx = cos(s * transformCur[0]);
        float sry = sin(s * transformCur[1]);
        float cry = cos(s * transformCur[1]);
        float srz = sin(s * transformCur[2]);
        float crz = cos(s * transformCur[2]);
        float tx = s * transformCur[3];
        float ty = s * transformCur[4];
        float tz = s * transformCur[5];

        int laserCloudSelNum = laserCloudOri->size();
        // ROS_INFO_STREAM("Correspondence number: "<< laserCloudSelNum);
        if (laserCloudSelNum < 30) {
            ROS_WARN("Only %d point pairs, Not enough!",laserCloudSelNum);
            // ros::shutdown();
            return false;
        }

        cv::Mat matA(laserCloudSelNum, 6, CV_32F, cv::Scalar::all(0));
        cv::Mat matAt(6, laserCloudSelNum, CV_32F, cv::Scalar::all(0));
        cv::Mat matAtA(6, 6, CV_32F, cv::Scalar::all(0));
        cv::Mat matB(laserCloudSelNum, 1, CV_32F, cv::Scalar::all(0));
        cv::Mat matAtB(6, 1, CV_32F, cv::Scalar::all(0));
        cv::Mat matX(6, 1, CV_32F, cv::Scalar::all(0));

        PointType pointOri, coeff;

        for (int i = 0; i < laserCloudSelNum; i++) 
        {
            pointOri = laserCloudOri->points[i];
            coeff = coeffSel->points[i];
            coeff.intensity = coeffSel->points[i].intensity;


            float arx = (-s*crx*sry*srz*pointOri.x + s*crx*crz*sry*pointOri.y + s*srx*sry*pointOri.z 
                      + s*tx*crx*sry*srz - s*ty*crx*crz*sry - s*tz*srx*sry) * coeff.x
                      + (s*srx*srz*pointOri.x - s*crz*srx*pointOri.y + s*crx*pointOri.z
                      + s*ty*crz*srx - s*tz*crx - s*tx*srx*srz) * coeff.y
                      + (s*crx*cry*srz*pointOri.x - s*crx*cry*crz*pointOri.y - s*cry*srx*pointOri.z
                      + s*tz*cry*srx + s*ty*crx*cry*crz - s*tx*crx*cry*srz) * coeff.z;

            float ary = ((-s*crz*sry - s*cry*srx*srz)*pointOri.x 
                      + (s*cry*crz*srx - s*sry*srz)*pointOri.y - s*crx*cry*pointOri.z 
                      + tx*(s*crz*sry + s*cry*srx*srz) + ty*(s*sry*srz - s*cry*crz*srx) 
                      + s*tz*crx*cry) * coeff.x
                      + ((s*cry*crz - s*srx*sry*srz)*pointOri.x 
                      + (s*cry*srz + s*crz*srx*sry)*pointOri.y - s*crx*sry*pointOri.z
                      + s*tz*crx*sry - ty*(s*cry*srz + s*crz*srx*sry) 
                      - tx*(s*cry*crz - s*srx*sry*srz)) * coeff.z;

            float arz = ((-s*cry*srz - s*crz*srx*sry)*pointOri.x + (s*cry*crz - s*srx*sry*srz)*pointOri.y
                      + tx*(s*cry*srz + s*crz*srx*sry) - ty*(s*cry*crz - s*srx*sry*srz)) * coeff.x
                      + (-s*crx*crz*pointOri.x - s*crx*srz*pointOri.y
                      + s*ty*crx*srz + s*tx*crx*crz) * coeff.y
                      + ((s*cry*crz*srx - s*sry*srz)*pointOri.x + (s*crz*sry + s*cry*srx*srz)*pointOri.y
                      + tx*(s*sry*srz - s*cry*crz*srx) - ty*(s*crz*sry + s*cry*srx*srz)) * coeff.z;

            float atx = -s*(cry*crz - srx*sry*srz) * coeff.x + s*crx*srz * coeff.y 
                      - s*(crz*sry + cry*srx*srz) * coeff.z;
  
            float aty = -s*(cry*srz + crz*srx*sry) * coeff.x - s*crx*crz * coeff.y 
                      - s*(sry*srz - cry*crz*srx) * coeff.z;
  
            float atz = s*crx*sry * coeff.x - s*srx * coeff.y - s*crx*cry * coeff.z;

            float d2 = coeff.intensity;

            matA.at<float>(i, 0) = arx;
            matA.at<float>(i, 1) = ary;
            matA.at<float>(i, 2) = arz;
            matA.at<float>(i, 3) = atx;
            matA.at<float>(i, 4) = aty;
            matA.at<float>(i, 5) = atz;
            matB.at<float>(i, 0) = -0.05 * d2;
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
            float eignThre[6] = {10, 10, 10, 10, 10, 10}; // why 10 here?
            for (int i = 5; i >= 0; i--) { // 有一个维度约束不够
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

        transformCur[0] += matX.at<float>(0, 0);
        transformCur[1] += matX.at<float>(1, 0);
        transformCur[2] += matX.at<float>(2, 0);
        transformCur[3] += matX.at<float>(3, 0);
        transformCur[4] += matX.at<float>(4, 0);
        transformCur[5] += matX.at<float>(5, 0);
        // printTrans(transformCur);
        float deltaR = sqrt(
                            pow(pcl::rad2deg(matX.at<float>(0, 0)), 2) +
                            pow(pcl::rad2deg(matX.at<float>(1, 0)), 2) +
                            pow(pcl::rad2deg(matX.at<float>(2, 0)), 2));
        float deltaT = sqrt(
                            pow(matX.at<float>(3, 0) * 100, 2) +
                            pow(matX.at<float>(4, 0) * 100, 2) +
                            pow(matX.at<float>(5, 0) * 100, 2));

        // ROS_INFO_STREAM("optimization took "<<opt.toc()<<"ms"); // < 1ms
        if (deltaR < deltaRthre && deltaT < deltaTthre) {
            return true; // converged
        }
        return false; // keep optimizing
    }

    void scan2MapOptimization()
    {
        if (laserCloudCornerCurDSNum > odomEdgeFeatureMinValidNum && laserCloudSurfCurDSNum > odomSurfFeatureMinValidNum)
        {
            int iterCount = 0;
            for (; iterCount < odomOptIteration; iterCount++)
            {
                laserCloudOri->clear();
                coeffSel->clear();

                TicToc getJacobian;
                cornerOptimization(iterCount);
                surfOptimization(iterCount);
                combineOptimizationCoeffs();
                // ROS_INFO_STREAM("Getting jacobian takes: "<<getJacobian.toc()<<"ms");
                // string strStart = to_string(iterCount)+": ";
                // printTrans(strStart, transformCur);
                if (LMOptimization(iterCount) == true){
                    break;              
                }
            }
            if (isDegenerate)             ROS_WARN("isDegenerate!");
            if (iterCount == optIteration) 
                    ROS_WARN("Odometty solution won't converge!"); 
        }
        else{
            ROS_WARN("Not enough features! Only %d edge and %d planar features available.", laserCloudCornerCurDSNum, laserCloudSurfCurDSNum);
        }
    }

    void checkSystemInitialization(){
        // pcl::copyPointCloud(*laserCloudCornerCur,*laserCloudCornerLast);
        // pcl::copyPointCloud(*laserCloudSurfCur,*laserCloudSurfLast);
        pcl::PointCloud<PointType>::Ptr laserCloudTempC = laserCloudCornerCur;
        laserCloudCornerCur = laserCloudCornerLast; // necessary? YES!
        laserCloudCornerLast = laserCloudTempC;

        pcl::PointCloud<PointType>::Ptr laserCloudTempS = laserCloudSurfCur;
        laserCloudSurfCur = laserCloudSurfLast;
        laserCloudSurfLast = laserCloudTempS;

        kdtreeCornerLast->setInputCloud(laserCloudCornerLast);
        kdtreeSurfLast->setInputCloud(laserCloudSurfLast);
        systemInitedLM = true;
    }

    void updateInitialGuess(){
        // transformCur not changed: csmm
    }


    void publishOdometry(){
        // printTrans(transformSum);
        geometry_msgs::Quaternion geoQuat = tf::createQuaternionMsgFromRollPitchYaw(transformSum[2], -transformSum[0], -transformSum[1]);

        laserOdometry.header.stamp = cloudHeader.stamp;
        laserOdometry.pose.pose.orientation.x = -geoQuat.y;
        laserOdometry.pose.pose.orientation.y = -geoQuat.z;
        laserOdometry.pose.pose.orientation.z = geoQuat.x;
        laserOdometry.pose.pose.orientation.w = geoQuat.w;
        laserOdometry.pose.pose.position.x = transformSum[3];
        laserOdometry.pose.pose.position.y = transformSum[4];
        laserOdometry.pose.pose.position.z = transformSum[5];
        pubLidarOdometry.publish(laserOdometry);

    //    // publish tf
    //     laserOdometryTrans.stamp_ = cloudHeader.stamp;
    //     laserOdometryTrans.setRotation(tf::Quaternion(-geoQuat.y, -geoQuat.z, geoQuat.x, geoQuat.w));
    //     laserOdometryTrans.setOrigin(tf::Vector3(transformSum[3], transformSum[4], transformSum[5]));
    //     tfBroadcaster.sendTransform(laserOdometryTrans);

        // for rviz 
        // printTrans(transformSum);
        geometry_msgs::PoseStamped pose_stamped; // notice here it is not nav_msgs::Odometry!
        pose_stamped.header.stamp = cloudHeader.stamp;
        pose_stamped.header.frame_id = mapFrame;
        pose_stamped.pose.position.x = transformSum[3];
        pose_stamped.pose.position.y = transformSum[4];
        pose_stamped.pose.position.z = transformSum[5];
        pose_stamped.pose.orientation = laserOdometry.pose.pose.orientation;
        odometryPath.poses.push_back(pose_stamped);
        // Path message also needs stamp and frame id
        odometryPath.header.stamp = cloudHeader.stamp;
        odometryPath.header.frame_id = mapFrame;
        pubLidarPath.publish(odometryPath);

    }

    void laserCloudInfoHandler(const kloam::cloud_infoConstPtr& msgIn)
    {
        std::lock_guard<std::mutex> lock(mtx);

        TicToc odometry;
        cloudHeader = msgIn->header;

        // extract info and feature cloud
        cloudInfo = *msgIn;
        pcl::fromROSMsg(msgIn->cloud_corner,  *laserCloudCornerCur);
        pcl::fromROSMsg(msgIn->cloud_surface, *laserCloudSurfCur);
        pcl::fromROSMsg(msgIn->cloud_corner_sharp,  *laserCloudCornerCurDS);
        pcl::fromROSMsg(msgIn->cloud_surface_flat, *laserCloudSurfCurDS);
        // string strStart = "start: ";
        // printTrans(strStart, transformCur);
        if (!systemInitedLM) 
        {
            checkSystemInitialization();
            return;
        }
        // downsampleCurrentScan(); // or else it will be as slow as the mapping module
        laserCloudCornerCurDSNum = laserCloudCornerCurDS->size();
        laserCloudSurfCurDSNum = laserCloudSurfCurDS->size();
        if (laserCloudCornerCur->points.empty() || laserCloudSurfCur->points.empty() ||
                laserCloudCornerCurDS->points.empty() || laserCloudSurfCurDS->points.empty())
        {
            ROS_WARN("How come there is suddenly an empty frame?");
            return; // nclt20120108,YES,you!
        }

        if (laserCloudCornerCur->size() < 100 || laserCloudSurfCur->size() < 100) // corner should be at least 20*6*32
        {
            ROS_WARN("How come there is suddenly a degraded frame?");
            return; // nclt20120115 ~@4100,YES,you!
        }
        _pointSearchCornerInd1.resize(laserCloudCornerCurDSNum);
        _pointSearchCornerInd2.resize(laserCloudCornerCurDSNum);
        _pointSearchSurfInd1.resize(laserCloudSurfCurDSNum);
        _pointSearchSurfInd2.resize(laserCloudSurfCurDSNum);
        _pointSearchSurfInd3.resize(laserCloudSurfCurDSNum);

        // ROS_INFO_STREAM("Before sampling: "<<laserCloudCornerCur->size()<<" "<<laserCloudSurfCur->size());
        // ROS_INFO_STREAM("After sampling: "<<laserCloudCornerCurDSNum<<" "<<laserCloudSurfCurDSNum);
        
        updateInitialGuess(); // no odom guess needed for now

        scan2MapOptimization();

        integrateTransformation();

        publishOdometry();
        
        // // this way also works!
        int laserCloudCornerCurNum = laserCloudCornerCur->size();
        for (int i = 0; i < laserCloudCornerCurNum; i++) {
            TransformToEnd(&laserCloudCornerCur->points[i], &laserCloudCornerCur->points[i]);
        }


        int laserCloudSurfCurNum = laserCloudSurfCur->points.size();
        for (int i = 0; i < laserCloudSurfCurNum; i++) {
            TransformToEnd(&laserCloudSurfCur->points[i], &laserCloudSurfCur->points[i]);
        }

        pcl::copyPointCloud(*laserCloudCornerCur,*laserCloudCornerLast);
        pcl::copyPointCloud(*laserCloudSurfCur,*laserCloudSurfLast);
        kdtreeCornerLast.reset(new pcl::KdTreeFLANN<PointType>());
        kdtreeSurfLast.reset(new pcl::KdTreeFLANN<PointType>());
        TicToc kdSet;
        kdtreeCornerLast->setInputCloud(laserCloudCornerLast);
        kdtreeSurfLast->setInputCloud(laserCloudSurfLast);
        odometryTime.push_back(odometry.toc());
        // ROS_INFO_STREAM("Setting up kdtree takes: "<<kdSet.toc()<<" ms"); // < 2ms
        // ROS_INFO_STREAM("Odometry takes "<<odometryTime[odometryTime.size()-1]<<" ms");
    }
    void downsampleCurrentScan()
    {
        // Downsample cloud from current scan
        // cout<<"Before sampling: "<<laserCloudCornerCur->size()<<" "<<laserCloudSurfCur->size()<<endl;
        downSizeFilterCorner.setInputCloud(laserCloudCornerCur);
        downSizeFilterCorner.filter(*laserCloudCornerCurDS);
        downSizeFilterSurf.setInputCloud(laserCloudSurfCur);
        downSizeFilterSurf.filter(*laserCloudSurfCurDS);
    }

};




int main(int argc, char** argv)
{
    ros::init(argc, argv, "kloam");
    lidarOdometry LO;   
    ROS_INFO("\033[1;32m----> Lidar Odometry Started.\033[0m");
    ros::spin();
    float mean = 0; 
    int sizeO = LO.odometryTime.size();
    for (int i=0; i<sizeO; i++)
        mean += LO.odometryTime[i];
    cout<<"Average time consumed by odometry is : "<<mean/sizeO<<endl;
    return 0;
}
