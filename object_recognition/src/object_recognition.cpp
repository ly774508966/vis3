#include "object_recognition.h"
#include <dirent.h>
#include <pcl/io/obj_io.h>
#include <pcl/segmentation/extract_clusters.h>
#include <pcl/registration/icp.h>
Object_recognition::Object_recognition(const std::string object_folder_path)
{    
    setObjects(object_folder_path);
}

void Object_recognition::setObjects(const std::string object_folder_path)
{
    PCL_INFO("Loading objects from [%s]\n",object_folder_path.c_str());
    struct dirent *pDirent;
    DIR *pDir;
    pDir = opendir (object_folder_path.c_str());
    if (pDir == NULL) {
        printf ("Cannot open directory '%s'\n", object_folder_path.c_str());
        return;
    }
    vector<PointCloudT::Ptr> object_clouds; // for debugging
    char *obj_file_end = "obj"; // "mtl"
    while ((pDirent = readdir(pDir)) != NULL) {
        if(!std::strcmp( pDirent->d_name, "." ) || !std::strcmp( pDirent->d_name, "..")
                || !std::strcmp( pDirent->d_name,"./coke_can.obj.obj"))
            continue;
        std::string fn(pDirent->d_name);
        if(fn.substr(fn.find_last_of(".") + 1) == obj_file_end) {
          printf ("Loading [%s]\n", pDirent->d_name);
          PointCloudT::Ptr tmpCloud(new PointCloudT);
          pcl::io::loadOBJFile<PointT>(object_folder_path+"/"+std::string(pDirent->d_name), *tmpCloud);
          objects.push_back(Feature_cloud(tmpCloud));
          object_clouds.push_back(tmpCloud);
        }
    }
    closedir (pDir);
}

void Object_recognition::getObjects(const PointCloudT::Ptr& scene, std::vector<Result>& object_poses)
{    
    PointCloudT::Ptr cloud(new PointCloudT);
    pcl::copyPointCloud(*scene,*cloud);
    getObjectPointsOnPlane(scene, cloud);
    PCL_DEBUG("Segment objects");
    vector<Feature_cloud> scene_objects;
    segmentSceneInObjects(cloud, scene_objects);
    PCL_DEBUG("Align objects");
    alignment(scene_objects, object_poses);
}

void Object_recognition::segmentSceneInObjects( const PointCloudT::Ptr& cloud, std::vector<Feature_cloud>& scene_objects)
{

    // Creating the KdTree object for the search method of the extraction
    pcl::search::KdTree<pcl::PointXYZ>::Ptr tree (new pcl::search::KdTree<pcl::PointXYZ>);
    tree->setInputCloud (cloud);

    std::vector<pcl::PointIndices> cluster_indices;
    pcl::EuclideanClusterExtraction<pcl::PointXYZ> ec;
    ec.setClusterTolerance (0.02); // 2cm
    ec.setMinClusterSize (100);
    ec.setMaxClusterSize (25000);
    ec.setSearchMethod (tree);
    ec.setInputCloud (cloud);
    ec.extract (cluster_indices);

    int j = 0;
    for (std::vector<pcl::PointIndices>::const_iterator it = cluster_indices.begin (); it != cluster_indices.end (); ++it)
    {
      pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_cluster (new pcl::PointCloud<pcl::PointXYZ>);
      for (std::vector<int>::const_iterator pit = it->indices.begin (); pit != it->indices.end (); ++pit)
        cloud_cluster->points.push_back (cloud->points[*pit]); //*
      cloud_cluster->width = cloud_cluster->points.size ();
      cloud_cluster->height = 1;
      cloud_cluster->is_dense = true;
      scene_objects.push_back(Feature_cloud(cloud_cluster));
      std::cout << "PointCloud representing the Cluster: " << cloud_cluster->points.size () << " data points." << std::endl;
      j++;
    }
}

void Object_recognition::showPointCloud(PointCloudT::Ptr cloud, char* name)
{
    pcl::visualization::PCLVisualizer::Ptr viewer (new pcl::visualization::PCLVisualizer (name));
    viewer->setBackgroundColor (0, 0, 0);
    viewer->addPointCloud<PointT> (cloud, "sample cloud");
    viewer->setPointCloudRenderingProperties (pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 1, "sample cloud");
    viewer->addCoordinateSystem (0.1);
    viewer->initCameraParameters();
    viewer->setSize(640, 480);
    while (!viewer->wasStopped())
    {
        viewer->spinOnce();
        boost::this_thread::sleep (boost::posix_time::microseconds (100000));
        // Do nothing but wait.
    }
}

void Object_recognition::alignment(const std::vector<Feature_cloud>& scene_objects, std::vector<Result>& objects_pose)
{
    double min_sample_distance_ (0.05f);
    double max_correspondence_distance_ (0.01f*0.01f);
    double nr_iterations_ (500);
    pcl::SampleConsensusInitialAlignment<PointT, PointT, pcl::SHOT352> sac_ia;
    sac_ia.setMinSampleDistance (min_sample_distance_);
    sac_ia.setMaxCorrespondenceDistance (max_correspondence_distance_);
    sac_ia.setMaximumIterations (nr_iterations_);
    for(size_t j=0; j<scene_objects.size(); j++)
    {
        sac_ia.setInputTarget(scene_objects[j].getCloud());
        //showPointCloud(scene_objects[j].getCloud(), "segmented object point cloud");
        sac_ia.setTargetFeatures(scene_objects[j].getLocalFeatures());
        // Find the template with the best (lowest) fitness score
        Result best_object_match;
        best_object_match.score = std::numeric_limits<float>::infinity ();
        int best_object_i = 0;
        for(size_t i=0; i<objects.size(); i++)
        {
            sac_ia.setInputCloud(objects[i].getCloud());
            sac_ia.setSourceFeatures(objects[i].getLocalFeatures());
            pcl::PointCloud<pcl::PointXYZ> registration_output;
            sac_ia.align (registration_output);
            double score = (float) sac_ia.getFitnessScore(max_correspondence_distance_);
            if (score < best_object_match.score)
            {
              best_object_i = i;
              best_object_match.score = score;
              best_object_match.transformation = sac_ia.getFinalTransformation();
            }
        }

        if(best_object_match.score < std::numeric_limits<float>::infinity ())// TODO: test for minimums score 0.00002 is good
        {
            refineAlignment(objects[best_object_i].getCloud(), scene_objects[j].getCloud(), best_object_match);
            objects_pose.push_back(best_object_match);
        }

    }
}

void
Object_recognition::refineAlignment(PointCloudT::Ptr query, PointCloudT::Ptr target, Result res)
{
    int icpIterations(100);
    int inlierThreshold(5);
    pcl::IterativeClosestPoint<PointT,PointT> icp;
    icp.setInputSource(query);
    icp.setInputTarget(target);
    icp.setMaximumIterations(icpIterations);
    icp.setMaxCorrespondenceDistance(inlierThreshold);
    pcl::PointCloud<PointT> tmp;
    icp.align(tmp, res.transformation);
    if(icp.hasConverged()) {
        res.transformation = icp.getFinalTransformation();
        res.score = icp.getFitnessScore();
    } else {
        PCL_WARN("ICP failed!");
    }
    std::cout << "has converged:" << icp.hasConverged() << " score: " <<
    icp.getFitnessScore() << std::endl;
    std::cout << icp.getFinalTransformation() << std::endl;
}

bool
Object_recognition::getObjectPointsOnPlane(const pcl::PointCloud<pcl::PointXYZ>::Ptr& scene, pcl::PointCloud<pcl::PointXYZ>::Ptr& objects)
{  
    // Create the filtering object
    pcl::PointCloud<pcl::PointXYZ>::Ptr scene_filtered (new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PassThrough<pcl::PointXYZ> pass;
    pass.setInputCloud (scene);
    pass.setFilterFieldName ("z");
    pass.setFilterLimits (0.0, 2.0);
    //pass.setFilterLimitsNegative (true);
    pass.filter (*scene_filtered);

    bool plane_found;
    pcl::PointCloud<pcl::PointXYZ>::Ptr plane(new pcl::PointCloud<pcl::PointXYZ>);

    // Get the plane model, if present.
    pcl::ModelCoefficients::Ptr coefficients(new pcl::ModelCoefficients);
    pcl::SACSegmentation<pcl::PointXYZ> segmentation;
    segmentation.setInputCloud(scene_filtered);
    segmentation.setModelType(pcl::SACMODEL_PLANE);
    segmentation.setMethodType(pcl::SAC_RANSAC);
    segmentation.setDistanceThreshold(0.005);
    segmentation.setOptimizeCoefficients(true);
    segmentation.setMaxIterations(100);
    pcl::PointIndices::Ptr planeIndices(new pcl::PointIndices);
    segmentation.segment(*planeIndices, *coefficients);

    if (planeIndices->indices.size() == 0) {
        std::cout << "Could not find a plane in the scene." << std::endl;
        plane_found = false;
    }
    else
    {
        // Copy the points of the plane to a new cloud.
        pcl::ExtractIndices<pcl::PointXYZ> extract;
        extract.setInputCloud(scene_filtered);
        extract.setIndices(planeIndices);
        extract.filter(*plane);

        // Retrieve the convex hull.
        pcl::PointCloud<pcl::PointXYZ>::Ptr convexHull(new pcl::PointCloud<pcl::PointXYZ>);
        pcl::ConvexHull<pcl::PointXYZ> hull;
        hull.setInputCloud(plane);
        // Make sure that the resulting hull is bidimensional.
        hull.setDimension(2);
        hull.reconstruct(*convexHull);
        // Redundant check.
        if (hull.getDimension() == 2)
        {
            // Prism object.
            pcl::ExtractPolygonalPrismData<pcl::PointXYZ> prism;
            prism.setInputCloud(scene_filtered);
            prism.setInputPlanarHull(convexHull);
            // First parameter: minimum Z value. Set to 0, segments objects lying on the plane (can be negative).
            // Second parameter: maximum Z value, set to 10cm. Tune it according to the height of the objects you expect.
            prism.setHeightLimits(0.01f, 0.2f);
            pcl::PointIndices::Ptr objectIndices(new pcl::PointIndices);

            prism.segment(*objectIndices);

            // Get and show all points retrieved by the hull.
            extract.setIndices(objectIndices);
            extract.filter(*objects);
            /*
            // --------------------------------------------
            // -----Open 3D viewer and add point cloud-----
            // --------------------------------------------
            pcl::visualization::PCLVisualizer::Ptr viewer (new pcl::visualization::PCLVisualizer ("3D Viewer"));
            viewer->setBackgroundColor (0, 0, 0);
            viewer->addPointCloud<pcl::PointXYZ> (objects, "sample cloud");
            viewer->setPointCloudRenderingProperties (pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 1, "sample cloud");
            viewer->addCoordinateSystem (0.1);
            viewer->initCameraParameters();
            viewer->setSize(640, 480);
            while (!viewer->wasStopped())
            {
                viewer->spinOnce();
                boost::this_thread::sleep (boost::posix_time::microseconds (100000));
                // Do nothing but wait.
            }
            /*
            pcl::visualization::CloudViewer viewerObjects("Objects on table");
            viewerObjects.showCloud(objects,"objects");
            while (!viewerObjects.wasStopped())
            {
                // Do nothing but wait.
            }
            */
            plane_found = true;
        }
        else {
            std::cout << "The chosen hull is not planar." << std::endl;
            plane_found = false;
        }
    }
    return plane_found;
}