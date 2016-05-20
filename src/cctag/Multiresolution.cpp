#define png_infopp_NULL (png_infopp)NULL
#define int_p_NULL (int*)NULL
#include <boost/gil/extension/io/png_io.hpp>
#include <boost/gil/image_view_factory.hpp>
#include <limits>

#include <cctag/Multiresolution.hpp>
#include <cctag/utils/VisualDebug.hpp>
#include <cctag/utils/FileDebug.hpp>
#include <cctag/Vote.hpp>
#include <cctag/EllipseGrowing.hpp>
#include <cctag/geometry/EllipseFromPoints.hpp>
#include <cctag/Fitting.hpp>
#include <cctag/Canny.hpp>
#include <cctag/Detection.hpp>
#include <cctag/utils/Talk.hpp> // for DO_TALK macro

#include <boost/gil/image_view.hpp>
#include <boost/gil/typedefs.hpp>
#include <boost/gil/image_view_factory.hpp>
#include <boost/timer.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <cmath>
#include <sstream>
#include <fstream>
#include <map>

#ifdef WITH_CUDA
#include <cuda_runtime.h> // only for debugging!!!
#include "cuda/tag.h"
#endif


namespace cctag
{

/* @brief Add markers from a list to another, deleting duplicates.
 *
 *
 */

static bool intersectLineToTwoEllipses(
        std::ssize_t y,
        const numerical::geometry::Ellipse & qIn,
        const numerical::geometry::Ellipse & qOut,
        const EdgePointCollection & edgeCollection,
        std::list<EdgePoint*> & pointsInHull)
{
  const auto& edgesMap = edgeCollection.map();
  std::vector<float> intersectionsOut = numerical::geometry::intersectEllipseWithLine(qOut, y, true);
  std::vector<float> intersectionsIn = numerical::geometry::intersectEllipseWithLine(qIn, y, true);
  BOOST_ASSERT(intersectionsOut.size() <= 2);
  BOOST_ASSERT(intersectionsIn.size() <= 2);
  if ((intersectionsOut.size() == 2) && (intersectionsIn.size() == 2))
  {
    //@todo@Lilian, in/out the edgeMap
    std::ssize_t begin1 = std::max(0, (int) intersectionsOut[0]);
    std::ssize_t end1 = std::min((int) edgesMap.shape()[0] - 1, (int) intersectionsIn[0]);

    std::ssize_t begin2 = std::max(0, (int) intersectionsIn[1]);
    std::ssize_t end2 = std::min((int) edgesMap.shape()[0] - 1, (int) intersectionsOut[1]);

    for (int x = begin1; x <= end1; ++x)
    {
      EdgePoint* edgePoint = edgeCollection(x,y);
      if (edgePoint)
      {
        // Check that the gradient is opposed to the ellipse's center before pushing it.
        Eigen::Vector2f centerToPoint;
        centerToPoint(0) = qIn.center().x() - (*edgePoint).x();
        centerToPoint(1) = qIn.center().y() - (*edgePoint).y();
        
        if (edgePoint->gradient().dot(centerToPoint) < 0)
        {
          pointsInHull.push_back(edgePoint);
        }
      }
    }
    for (int x = begin2; x <= end2; ++x)
    {
      EdgePoint* edgePoint = edgeCollection(x,y);
      if (edgePoint)
      {
        // Check that the gradient is opposed to the ellipse's center before pushing it.
        Eigen::Vector2f centerToPoint;
        centerToPoint(0) = qIn.center().x() - (*edgePoint).x();
        centerToPoint(1) = qIn.center().y() - (*edgePoint).y();
        
        if (edgePoint->gradient().dot(centerToPoint) < 0)
        {
          pointsInHull.push_back(edgePoint);
        }
      }
    }
  }
  else if ((intersectionsOut.size() == 2) && (intersectionsIn.size() <= 1))
  {
    std::ssize_t begin = std::max(0, (int) intersectionsOut[0]);
    std::ssize_t end = std::min((int) edgesMap.shape()[0] - 1, (int) intersectionsOut[1]);

    for (int x = begin; x <= end; ++x)
    {
      EdgePoint* edgePoint = edgeCollection(x,y);
      if (edgePoint)
      {
        // Check that the gradient is opposed to the ellipse's center before pushing it.
        
        Eigen::Vector2f centerToPoint;
        centerToPoint(0) = qIn.center().x() - (*edgePoint).x();
        centerToPoint(1) = qIn.center().y() - (*edgePoint).y();
        
        if (edgePoint->gradient().dot(centerToPoint) < 0)
        {
          pointsInHull.push_back(edgePoint);
        }
      }
    }
  }
  else if ((intersectionsOut.size() == 1) && (intersectionsIn.size() == 0))
  {
    if ((intersectionsOut[0] >= 0) && (intersectionsOut[0] < edgesMap.shape()[0]))
    {
      EdgePoint* edgePoint = edgeCollection(intersectionsOut[0],y);
      if (edgePoint)
      {
        // Check that the gradient is opposed to the ellipse's center before pushing it.
        Eigen::Vector2f centerToPoint;
        centerToPoint(0) = qIn.center().x() - (*edgePoint).x();
        centerToPoint(1) = qIn.center().y() - (*edgePoint).y();
        
        if (edgePoint->gradient().dot(centerToPoint) < 0)
        {
          pointsInHull.push_back(edgePoint);
        }
      }
    }
  }
  else //if( intersections.size() == 0 )
  {
    return false;
  }
  return true;
}

static void selectEdgePointInEllipticHull(
        const EdgePointCollection & edgeCollection,
        const numerical::geometry::Ellipse & outerEllipse,
        float scale,
        std::list<EdgePoint*> & pointsInHull)
{
  numerical::geometry::Ellipse qIn, qOut;
  computeHull(outerEllipse, scale, qIn, qOut);

  const float yCenter = outerEllipse.center().y();
  const auto& edgesMap = edgeCollection.map();

  int maxY = std::max(int(yCenter), 0);
  int minY = std::min(int(yCenter), int(edgesMap.shape()[1]) - 1);

  // Visit the bottom part of the ellipse
  for (std::ssize_t y = maxY; y < int( edgesMap.shape()[1]); ++y)
  {
    if (!intersectLineToTwoEllipses(y, qIn, qOut, edgeCollection, pointsInHull))
      break;
  }
  // Visit the upper part of the ellipse
  for (std::ssize_t y = minY; y >= 0; --y)
  {
    if (!intersectLineToTwoEllipses(y, qIn, qOut, edgeCollection, pointsInHull))
      break;
  }
}

void update(
        CCTag::List& markers,
        const CCTag& markerToAdd)
{
  bool flag = false;

  BOOST_FOREACH(CCTag & currentMarker, markers)
  {
    // If markerToAdd is overlapping with a marker contained in markers then
    //if (currentMarker.isOverlapping(markerToAdd)) // todo: clean
    if (currentMarker.isEqual(markerToAdd))       
    {
      if (markerToAdd.quality() > currentMarker.quality())
      {
        currentMarker = markerToAdd;
      }
      flag = true;
    }
  }
  // else push back in markers.
  if (!flag)
  {
    markers.push_back(new CCTag(markerToAdd));
  }
}

static void cctagMultiresDetection_inner(
        size_t                  i,
        CCTag::List&            pyramidMarkers,
        const cv::Mat&          imgGraySrc,
        Level*                  level,
        const std::size_t       frame,
        EdgePointCollection&    edgeCollection,
        popart::TagPipe*        cuda_pipe,
        const Parameters &      params,
        cctag::logtime::Mgmt*   durations )
{
    DO_TALK( CCTAG_COUT_OPTIM(":::::::: Multiresolution level " << i << "::::::::"); )

    // Data structure for getting vote winners
    std::vector<EdgePoint*> seeds;

    boost::posix_time::time_duration d;

#if defined(WITH_CUDA)
    // there is no point in measuring time in compare mode
    if( cuda_pipe ) {
      cuda_pipe->convertToHost(i, edgeCollection, seeds);
      if( durations ) {
          cudaDeviceSynchronize();
      }
      level->setLevel( cuda_pipe, params );

      CCTagVisualDebug::instance().setPyramidLevel(i);
    } else { // not cuda_pipe
#endif // defined(WITH_CUDA)
    edgesPointsFromCanny( edgeCollection,
                          level->getEdges(),
                          level->getDx(),
                          level->getDy());

    CCTagVisualDebug::instance().setPyramidLevel(i);

    // Voting procedure applied on every edge points.
    vote( edgeCollection,
          seeds,        // output
          level->getDx(),
          level->getDy(),
          params );
    
    if( seeds.size() > 1 ) {
        // Sort the seeds based on the number of received votes.
        std::sort(seeds.begin(), seeds.end(), receivedMoreVoteThan);
    }

#if defined(WITH_CUDA)
    } // not cuda_pipe
#endif // defined(WITH_CUDA)


    cctagDetectionFromEdges(
        pyramidMarkers,
        edgeCollection,
        level->getSrc(),
        seeds,
        frame, i, std::pow(2.0, (int) i), params,
        durations );

    CCTagVisualDebug::instance().initBackgroundImage(level->getSrc());
    std::stringstream outFilename2;
    outFilename2 << "viewLevel" << i;
    CCTagVisualDebug::instance().newSession(outFilename2.str());

    BOOST_FOREACH(const CCTag & marker, pyramidMarkers)
    {
        CCTagVisualDebug::instance().drawMarker(marker, false);
    }
}

void cctagMultiresDetection(
        CCTag::List& markers,
        const cv::Mat& imgGraySrc,
        const ImagePyramid& imagePyramid,
        const std::size_t   frame,
        popart::TagPipe*    cuda_pipe,
        const Parameters&   params,
        cctag::logtime::Mgmt* durations )
{
  //	* For each pyramid level:
  //	** launch CCTag detection based on the canny edge detection output.

  std::map<std::size_t, CCTag::List> pyramidMarkers;
  std::vector<EdgePointCollection> vEdgePointCollections(params._numberOfProcessedMultiresLayers);

  BOOST_ASSERT( params._numberOfMultiresLayers - params._numberOfProcessedMultiresLayers >= 0 );
  for ( std::size_t i = 0 ; i < params._numberOfProcessedMultiresLayers; ++i ) {
    pyramidMarkers.insert( std::pair<std::size_t, CCTag::List>( i, CCTag::List() ) );

    cctagMultiresDetection_inner( i,
                                  pyramidMarkers[i],
                                  imgGraySrc,
                                  imagePyramid.getLevel(i),
                                  frame,
                                  vEdgePointCollections.back(),
                                  cuda_pipe,
                                  params,
                                  durations );
  }
  if( durations ) durations->log( "after cctagMultiresDetection_inner" );
  
  // Delete overlapping markers while keeping the best ones.
  
  CCTag::List markersPrelim;
  
  BOOST_ASSERT( params._numberOfMultiresLayers - params._numberOfProcessedMultiresLayers >= 0 );
  for (std::size_t i = 0 ; i < params._numberOfProcessedMultiresLayers ; ++i)
  // set the _numberOfProcessedMultiresLayers <= _numberOfMultiresLayers todo@Lilian
  {
    CCTag::List & markersList = pyramidMarkers[i];

    BOOST_FOREACH(const CCTag & marker, markersList)
    {
        update(markersPrelim, marker);
    }
  }
  
  // todo: in which case is this float check required ?
  for(const CCTag & marker : markersPrelim)
  {
    update(markers, marker);
  }
  
  if( durations ) durations->log( "after update markers" );
  
  CCTagVisualDebug::instance().initBackgroundImage(imagePyramid.getLevel(0)->getSrc());
  CCTagVisualDebug::instance().writeLocalizationView(markers);

  // Final step: extraction of the detected markers in the original (scale) image.
  CCTagVisualDebug::instance().newSession("multiresolution");

  // Project markers from the top of the pyramid to the bottom (original image).
  BOOST_FOREACH(CCTag & marker, markers)
  {
    int i = marker.pyramidLevel();
    // if the marker has to be rescaled into the original image
    if (i > 0)
    {
      BOOST_ASSERT( i < params._numberOfMultiresLayers );
      float scale = marker.scale(); // pow( 2.0, (float)i );

      cctag::numerical::geometry::Ellipse rescaledOuterEllipse = marker.rescaledOuterEllipse();

      #ifdef CCTAG_OPTIM
        boost::posix_time::ptime t0(boost::posix_time::microsec_clock::local_time());
      #endif
      
      
      std::list<EdgePoint*> pointsInHull;
      selectEdgePointInEllipticHull(vEdgePointCollections[0], rescaledOuterEllipse, scale, pointsInHull);

      #ifdef CCTAG_OPTIM
        boost::posix_time::ptime t1(boost::posix_time::microsec_clock::local_time());
      #endif
      
      std::vector<EdgePoint*> rescaledOuterEllipsePoints;

      float SmFinal = 1e+10;
      
      cctag::outlierRemoval(
              pointsInHull,
              rescaledOuterEllipsePoints,
              SmFinal,
              20.0,
              NO_WEIGHT,
              60); 
      
      #ifdef CCTAG_OPTIM
        boost::posix_time::ptime t2(boost::posix_time::microsec_clock::local_time());
        boost::posix_time::time_duration d1 = t1 - t0;
        boost::posix_time::time_duration d2 = t2 - t1;
        CCTAG_COUT_OPTIM("Time in selectEdgePointInEllipticHull: " << d1.total_milliseconds() << " ms");
        CCTAG_COUT_OPTIM("Time in outlierRemoval: " << d2.total_milliseconds() << " ms");
      #endif
      
      try
      {
        numerical::ellipseFitting(rescaledOuterEllipse, rescaledOuterEllipsePoints);

        std::vector< DirectedPoint2d<Eigen::Vector3f> > rescaledOuterEllipsePointsDouble;// todo@Lilian : add a reserve
        std::size_t numCircles = params._nCrowns * 2;

        BOOST_FOREACH(EdgePoint * e, rescaledOuterEllipsePoints)
        {
          rescaledOuterEllipsePointsDouble.push_back(
                  DirectedPoint2d<Eigen::Vector3f>(e->x(), e->y(),
                  e->dX(),
                  e->dY())
          );
          
          CCTagVisualDebug::instance().drawPoint(Point2d<Eigen::Vector3f>(e->x(), e->y()), cctag::color_red);
        }
        //marker.setCenterImg(rescaledOuterEllipse.center());                                                                // todo
        marker.setCenterImg(cctag::Point2d<Eigen::Vector3f>(marker.centerImg().x() * scale, marker.centerImg().y() * scale));  // decide between these two lines
        marker.setRescaledOuterEllipse(rescaledOuterEllipse);
        marker.setRescaledOuterEllipsePoints(rescaledOuterEllipsePointsDouble);
      }
      catch (...)
      {
        // catch exception from ellipseFitting
      }
    }
    else
    {
      marker.setRescaledOuterEllipsePoints(marker.points().back());
    }
  }
  if( durations ) durations->log( "after marker projection" );
  
  // Log
  CCTagFileDebug::instance().newSession("data.txt");
  BOOST_FOREACH(const CCTag & marker, markers)
  {
    CCTagFileDebug::instance().outputMarkerInfos(marker);
  }
  
  // POP_LEAVE;
  
}

} // namespace cctag

