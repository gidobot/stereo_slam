#include "stereo_slam_base.h"
#include <boost/shared_ptr.hpp>
#include <boost/filesystem.hpp>
#include <libpq-fe.h>
#include <Eigen/Geometry>
#include <iostream>
#include <fstream>
#include "opencv2/core/core.hpp"
#include "opencv2/features2d/features2d.hpp"
#include "opencv2/nonfree/features2d.hpp"
#include "postgresql_interface.h"
#include "utils.h"

/** \brief Class constructor. Reads node parameters and initialize some properties.
  * @return 
  * \param nh public node handler
  * \param nhp private node handler
  */
stereo_slam::StereoSlamBase::StereoSlamBase(
  ros::NodeHandle nh, ros::NodeHandle nhp) : nh_(nh), nh_private_(nhp)
{
  // Read the node parameters
  readParameters();

  // Initialize the stereo slam
  initializeStereoSlam();
}

/** \brief Messages callback. This function is called when syncronized odometry and image
  * message are received.
  * @return 
  * \param odom_msg ros odometry message of type nav_msgs::Odometry
  * \param l_img left stereo image message of type sensor_msgs::Image
  * \param r_img right stereo image message of type sensor_msgs::Image
  * \param l_info left stereo info message of type sensor_msgs::CameraInfo
  * \param r_info right stereo info message of type sensor_msgs::CameraInfo
  */
void stereo_slam::StereoSlamBase::msgsCallback(
                                  const nav_msgs::Odometry::ConstPtr& odom_msg,
                                  const sensor_msgs::ImageConstPtr& l_img,
                                  const sensor_msgs::ImageConstPtr& r_img,
                                  const sensor_msgs::CameraInfoConstPtr& l_info,
                                  const sensor_msgs::CameraInfoConstPtr& r_info)
{
  // Check for vertex insertion block
  if (block_insertion_)
    return;

  // Set camera model
  if (first_message_)
  {
    stereo_camera_model_.fromCameraInfo(l_info, r_info);
    const cv::Mat P(3,4, CV_64FC1, const_cast<double*>(l_info->P.data()));
    camera_matrix_ = P.colRange(cv::Range(0,3)).clone();
  }

  // Get the current odometry for these images
  tf::Vector3 tf_trans( odom_msg->pose.pose.position.x,
                        odom_msg->pose.pose.position.y,
                        odom_msg->pose.pose.position.z);
  tf::Quaternion tf_q ( odom_msg->pose.pose.orientation.x,
                        odom_msg->pose.pose.orientation.y,
                        odom_msg->pose.pose.orientation.z,
                        odom_msg->pose.pose.orientation.w);

  tf::Transform current_pose(tf_q, tf_trans);
  tf::Transform corrected_pose = current_pose;

  // Compute the corrected pose with the optimized graph
  double pose_diff = -1.0;
  int last_vertex_idx = graph_optimizer_.vertices().size() - 1;
  if (pose_history_.size() > 0 && last_vertex_idx >= 0)
  {
    // Compute the tf between last original pose before optimization and current.
    tf::Transform last_original_pose = pose_history_.at(last_vertex_idx);
    tf::Transform diff = last_original_pose.inverse() * current_pose;

    // Get the last optimized pose
    g2o::VertexSE3* last_vertex =  dynamic_cast<g2o::VertexSE3*>
          (graph_optimizer_.vertices()[last_vertex_idx]);
    tf::Transform last_optimized_pose = stereo_slam::Utils::getVertexPose(last_vertex);

    // Compute the corrected pose
    corrected_pose = last_optimized_pose * diff;

    // Compute the absolute pose difference
    pose_diff = stereo_slam::Utils::poseDiff(pose_history_.at(last_vertex_idx), current_pose);
  }

  // Check if difference between images is larger than minimum displacement
  if (pose_diff > params_.min_displacement || first_message_)
  {   
    // Convert message to cv::Mat
    cv_bridge::CvImagePtr l_ptr, r_ptr;
    try
    {
      l_ptr = cv_bridge::toCvCopy(l_img, enc::BGR8);
      r_ptr = cv_bridge::toCvCopy(r_img, enc::BGR8);
    }
    catch (cv_bridge::Exception& e)
    {
      ROS_ERROR("[StereoSlam:] cv_bridge exception: %s", e.what());
      return;
    }

    // Insert this vertex into the graph database
    if(vertexInsertion(l_ptr, r_ptr, corrected_pose))
    {
      // Save original pose history
      pose_history_.push_back(current_pose);
      pose_history_stamp_.push_back(odom_msg->header.stamp.toSec());
    }
  }

  // Publish slam (map)
  if (pose_pub_.getNumSubscribers() > 0)
  {
    geometry_msgs::PoseStamped pose_msg;
    geometry_msgs::Pose temp_pose;
    tf::poseTFToMsg(corrected_pose, temp_pose);
    pose_msg.header.stamp = odom_msg->header.stamp;
    pose_msg.header.frame_id = params_.map_frame_id;
    pose_msg.pose = temp_pose;
    pose_pub_.publish(pose_msg);
  }
}

/** \brief Timer callback. This function is called when update timer time is ellapsed.
  * @return 
  * \param event is the timer event object
  */
void stereo_slam::StereoSlamBase::timerCallback(const ros::WallTimerEvent& event)
{
  // Check if callback is currently executed
  if (block_update_)
    return;

  // Prevent for callback re-called
  block_update_ = true;

  // Update the graph and optimize it if new vertices have been inserted
  if (graphUpdater())
  {
    block_insertion_ = true;
    ROS_INFO_STREAM("[StereoSlam:] Optimizing global pose graph with " << 
                    graph_optimizer_.vertices().size() << " vertices...");
    graph_optimizer_.initializeOptimization();
    graph_optimizer_.optimize(params_.g2o_opt_max_iter);
    block_insertion_ = false;
    ROS_INFO("[StereoSlam:] Optimization done.");
  }

  // Save graph as odometry measurments in file
  if (params_.save_graph_to_file)
    saveGraph();

  block_update_ = false;
}

/** \brief Reads the stereo slam node parameters
  * @return
  */
void stereo_slam::StereoSlamBase::readParameters()
{
  Params stereo_slam_params;

  // G2O parameters
  nh_private_.getParam("update_rate", stereo_slam_params.update_rate);
  nh_private_.getParam("g2o_algorithm", stereo_slam_params.g2o_algorithm);
  nh_private_.getParam("g2o_opt_max_iter", stereo_slam_params.g2o_opt_max_iter);
  nh_private_.getParam("g2o_verbose", stereo_slam_params.g2o_verbose);

  // Graph operational parameters
  nh_private_.getParam("min_displacement", stereo_slam_params.min_displacement);
  nh_private_.getParam("max_candidate_threshold", stereo_slam_params.max_candidate_threshold);
  nh_private_.getParam("neighbor_offset", stereo_slam_params.neighbor_offset);
  nh_private_.getParam("save_graph_to_file", stereo_slam_params.save_graph_to_file);
  nh_private_.getParam("save_graph_images", stereo_slam_params.save_graph_images);
  nh_private_.getParam("files_path", stereo_slam_params.files_path);

  // Stereo vision parameters
  nh_private_.getParam("desc_type", stereo_slam_params.desc_type);
  nh_private_.getParam("descriptor_threshold", stereo_slam_params.descriptor_threshold);
  nh_private_.getParam("epipolar_threshold", stereo_slam_params.epipolar_threshold);
  nh_private_.getParam("matches_threshold", stereo_slam_params.matches_threshold);
  nh_private_.getParam("min_inliers", stereo_slam_params.min_inliers);
  nh_private_.getParam("allowed_reprojection_err", stereo_slam_params.allowed_reprojection_err);
  nh_private_.getParam("max_edge_err", stereo_slam_params.max_edge_err);
  nh_private_.getParam("stereo_vision_verbose", stereo_slam_params.stereo_vision_verbose);
  nh_private_.getParam("bucket_width", stereo_slam_params.bucket_width);
  nh_private_.getParam("bucket_height", stereo_slam_params.bucket_height);
  nh_private_.getParam("max_bucket_features", stereo_slam_params.max_bucket_features);

  // Topic parameters
  nh_private_.getParam("queue_size", stereo_slam_params.queue_size);
  nh_private_.getParam("map_frame_id", stereo_slam_params.map_frame_id);

  setParams(stereo_slam_params);

  // Topics subscriptions
  std::string odom_topic, left_topic, right_topic, left_info_topic, right_info_topic;
  nh_private_.param("odom_topic", odom_topic, std::string("/odometry"));
  nh_private_.param("left_topic", left_topic, std::string("/left/image_rect_color"));
  nh_private_.param("right_topic", right_topic, std::string("/right/image_rect_color"));
  nh_private_.param("left_info_topic", left_info_topic, std::string("/left/camera_info"));
  nh_private_.param("right_info_topic", right_info_topic, std::string("/right/camera_info"));
  image_transport::ImageTransport it(nh_);
  odom_sub_ .subscribe(nh_, odom_topic, 1);
  left_sub_ .subscribe(it, left_topic, 1);
  right_sub_.subscribe(it, right_topic, 1);
  left_info_sub_.subscribe(nh_, left_info_topic, 1);
  right_info_sub_.subscribe(nh_, right_info_topic, 1);
}

/** \brief Initializates the stereo slam node
  * @return
  */
bool stereo_slam::StereoSlamBase::initializeStereoSlam()
{
  // Operational initializations
  first_message_ = true;
  first_vertex_ = true;
  block_update_ = false;
  block_insertion_ = false;

  // Callback syncronization
  approximate_sync_.reset(new ApproximateSync(ApproximatePolicy(params_.queue_size),
                                  odom_sub_, 
                                  left_sub_, 
                                  right_sub_, 
                                  left_info_sub_, 
                                  right_info_sub_) );
  approximate_sync_->registerCallback(boost::bind(
      &stereo_slam::StereoSlamBase::msgsCallback,
      this, _1, _2, _3, _4, _5));

  // Advertise topics and services
  pose_pub_ = nh_private_.advertise<geometry_msgs::PoseStamped>("slam_pose", 1);

  // Initialize the g2o graph optimizer
  if (params_.g2o_algorithm == 0)
  {
    // Slam linear solver with gauss-newton
    SlamLinearSolver* linear_solver_ptr = new SlamLinearSolver();
    linear_solver_ptr->setBlockOrdering(false);
    SlamBlockSolver* block_solver_ptr = new SlamBlockSolver(linear_solver_ptr);
    g2o::OptimizationAlgorithmGaussNewton* solver_gauss_ptr = 
      new g2o::OptimizationAlgorithmGaussNewton(block_solver_ptr);
    graph_optimizer_.setAlgorithm(solver_gauss_ptr);
  }
  else if (params_.g2o_algorithm == 1)
  {
    // Linear solver with Levenberg
    g2o::BlockSolverX::LinearSolverType * linear_solver_ptr;
    linear_solver_ptr = new g2o::LinearSolverCholmod<g2o::BlockSolverX::PoseMatrixType>();
    g2o::BlockSolverX * solver_ptr = new g2o::BlockSolverX(linear_solver_ptr);
    g2o::OptimizationAlgorithmLevenberg * solver = 
      new g2o::OptimizationAlgorithmLevenberg(solver_ptr);
    graph_optimizer_.setAlgorithm(solver);
  }
  else
  {
    ROS_ERROR("[StereoSlam:] g2o_algorithm parameter must be 0 or 1.");
    return false;
  }  
  graph_optimizer_.setVerbose(params_.g2o_verbose);

  // Database initialization
  boost::shared_ptr<database_interface::PostgresqlDatabase> db_ptr_1( 
    new database_interface::PostgresqlDatabase( "localhost", 
                                                "5432", 
                                                "postgres", 
                                                "postgres", 
                                                "graph"));
  boost::shared_ptr<database_interface::PostgresqlDatabase> db_ptr_2( 
    new database_interface::PostgresqlDatabase( "localhost", 
                                                "5432", 
                                                "postgres", 
                                                "postgres", 
                                                "graph"));
  pg_db_ptr_thread_1_ = db_ptr_1;
  pg_db_ptr_thread_2_ = db_ptr_2;

  if (!pg_db_ptr_thread_1_->isConnected())
  {
    ROS_ERROR("[StereoSlam:] Database failed to connect");
  }
  else
  {
    ROS_INFO("[StereoSlam:] Database connected successfully!");

    // Database table creation. New connection is needed due to the interface design
    std::string conn_info = "host=localhost port=5432 user=postgres password=postgres dbname=graph";
    connection_init_= PQconnectdb(conn_info.c_str());
    if (PQstatus(connection_init_)!=CONNECTION_OK) 
    {
      ROS_ERROR_STREAM("[StereoSlam:] Database connection failed with error message: " <<
                        PQerrorMessage(connection_init_));
      return false;
    }
    else
    {
      // Drop the table (to start clean)
      std::string query_delete("DROP TABLE IF EXISTS graph");
      PQexec(connection_init_, query_delete.c_str());
      ROS_INFO("[StereoSlam:] graph table dropped successfully!");

      // Create the table (if no exists)
      std::string query_create("CREATE TABLE IF NOT EXISTS graph"
                        "( "
                          "id bigserial primary key, "
                          "keypoints double precision[][], "
                          "descriptors double precision[][], "
                          "points3d double precision[][] "
                        ")");
      PQexec(connection_init_, query_create.c_str());
      ROS_INFO("[StereoSlam:] graph table created successfully!");
    }
  }

  // Start timer for graph update
  timer_ = nh_.createWallTimer(ros::WallDuration(params_.update_rate), 
                               &stereo_slam::StereoSlamBase::timerCallback,
                               this);

  // Check parameters
  if (params_.matches_threshold < 5)
  {
    ROS_WARN("[StereoSlam:] Parameter 'matches_threshold' must be greater than 5. Set to 6.");
    params_.matches_threshold = 6;
    return false;
  }

  if (params_.files_path[params_.files_path.length()-1] != '/')
    params_.files_path += "/";

  std::string graph_image_dir = params_.files_path + "img/";
  if (params_.save_graph_images)
  {
    // Clear the directory if exists
    if (boost::filesystem::exists(graph_image_dir))
    {
      std::string rm = "rm -rf " + graph_image_dir;
      std::system(rm.c_str());
    }

    // Create the image directory again
    std::string mkdir = "mkdir " + graph_image_dir;
    std::system(mkdir.c_str());
  }

  // Remove previous saved files (if any)
  std::string vertices_file, edges_file;
  vertices_file = params_.files_path + "graph_vertices.txt";
  edges_file = params_.files_path + "graph_edges.txt";
  std::remove(vertices_file.c_str());
  std::remove(edges_file.c_str());

  return true;
}

/** \brief Save the optimized graph into a file with the same format than odometry_msgs.
  * It deletes all the file contents every time and re-write it with the last optimized.
  * @return
  */
bool stereo_slam::StereoSlamBase::saveGraph()
{
  std::string block_file, vertices_file, edges_file;
  vertices_file = params_.files_path + "graph_vertices.txt";
  edges_file = params_.files_path + "graph_edges.txt";
  block_file = params_.files_path + ".block.txt";

  // Create a blocking element
  std::fstream f_block(block_file.c_str(), std::ios::out | std::ios::trunc);

  // Open to append
  std::fstream f_vertices(vertices_file.c_str(), std::ios::out | std::ios::trunc);
  std::fstream f_edges(edges_file.c_str(), std::ios::out | std::ios::trunc);
  
  // Output the vertices file
  for (unsigned int i=0; i<graph_optimizer_.vertices().size(); i++)
  {
    // Compute timestamp
    double timestamp = pose_history_stamp_.at(i);

    g2o::VertexSE3* v = dynamic_cast<g2o::VertexSE3*>(graph_optimizer_.vertices()[i]);
    tf::Transform pose = stereo_slam::Utils::getVertexPose(v);
    f_vertices <<  std::setprecision(19) << 
          timestamp  << "," << 
          i << "," << 
          timestamp << "," << 
          params_.map_frame_id << "," << "," << 
          std::setprecision(6) << 
          pose.getOrigin().x() << "," << 
          pose.getOrigin().y() << "," << 
          pose.getOrigin().z() << "," << 
          pose.getRotation().x() << "," << 
          pose.getRotation().y() << "," << 
          pose.getRotation().z() << "," << 
          pose.getRotation().w() <<  std::endl;
  }
  f_vertices.close();

  // Output the edges file
  int counter = 0;
  for ( g2o::OptimizableGraph::EdgeSet::iterator it=graph_optimizer_.edges().begin();
        it!=graph_optimizer_.edges().end(); it++)
  {
    g2o::EdgeSE3* e = dynamic_cast<g2o::EdgeSE3*> (*it);
    if (e)
    {
      // Only take into account non-directed edges
      if (abs(e->vertices()[0]->id() - e->vertices()[1]->id()) > 1 )
      {
        g2o::VertexSE3* v_0 = dynamic_cast<g2o::VertexSE3*>(graph_optimizer_.vertices()[e->vertices()[0]->id()]);
        g2o::VertexSE3* v_1 = dynamic_cast<g2o::VertexSE3*>(graph_optimizer_.vertices()[e->vertices()[1]->id()]);
        tf::Transform pose_0 = stereo_slam::Utils::getVertexPose(v_0);
        tf::Transform pose_1 = stereo_slam::Utils::getVertexPose(v_1);

        f_edges << counter << "," << 
              std::setprecision(6) << 
              pose_0.getOrigin().x() << "," << 
              pose_0.getOrigin().y() << "," << 
              pose_0.getOrigin().z() << "," << 
              pose_1.getOrigin().x() << "," << 
              pose_1.getOrigin().y() << "," << 
              pose_1.getOrigin().z() <<  std::endl;
        counter++;
      }
    }
  }
  f_edges.close();

  // Un-block
  f_block.close();
  int ret_code = std::remove(block_file.c_str());
  if (ret_code != 0)
    ROS_ERROR("[StereoSlam:] Error deleting the blocking file.");   

  return true;
}