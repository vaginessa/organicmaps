#include "feature_rect.hpp"
#include "classificator.hpp"
#include "../geometry/point2d.hpp"
#include "../base/base.hpp"
#include "../base/buffer_vector.hpp"
#include "../base/macros.hpp"
#include "../base/stl_add.hpp"
#include "../std/utility.hpp"
#include "../std/vector.hpp"

namespace
{

class FeatureViewportEstimator
{
public:

  FeatureViewportEstimator()
  {
    m_TypeContinent   = GetType("place", "continent");
    m_TypeCountry     = GetType("place", "country");
    m_TypeCity        = GetType("place", "city");
    m_TypeCityCapital = GetType("place", "city", "capital");
    m_TypeTown        = GetType("place", "town");
    m_TypeVillage     = GetType("place", "village");
  }

  m2::RectD GetViewport(FeatureType const & feature) const
  {
    m2::RectD limitRect = feature.GetLimitRect(0);
    if (feature.GetFeatureType() != feature::GEOM_POINT)
      return limitRect;

    m2::PointD maxSizeMeters(0, 0);
    typedef buffer_vector<uint32_t, 8> TypeVectorT;
    TypeVectorT types;
    BackInsertFunctor<TypeVectorT> backInserter(types);
    feature.ForEachTypeRef(backInserter);
    for (size_t i = 0; i < types.size(); ++i)
    {
      m2::PointD const sizeMeters = GetSizeForType(types[i], feature);
      maxSizeMeters.x = max(maxSizeMeters.x, sizeMeters.x);
      maxSizeMeters.y = max(maxSizeMeters.y, sizeMeters.y);
    }

    m2::PointD const centerXY = limitRect.Center();
    return MercatorBounds::RectByCenterXYAndSizeInMeters(centerXY.x, centerXY.y,
                                                         maxSizeMeters.x, maxSizeMeters.y);
  }

  // Returns width and height (lon and lat) for a given type.
  m2::PointD GetSizeForType(uint32_t const type, FeatureType const & feature) const
  {
    static double const km = 1000.0;
    if (type == m_TypeContinent)
      return m2::PointD(5000*km, 5000*km);
    if (type == m_TypeCity || type == m_TypeCityCapital)
    {
      double const radius = sqrt(feature.GetPopulation() / 3000);
      return m2::PointD(radius*km, radius*km);
    }
    return m2::PointD(0, 0);
  }

private:

  static uint32_t GetType(string const & s1,
                          string const & s2 = string(),
                          string const & s3 = string())
  {
    vector<string> path;
    path.push_back(s1);
    if (!s2.empty()) path.push_back(s2);
    if (!s3.empty()) path.push_back(s3);
    return classif().GetTypeByPath(path);
  }

  uint32_t m_TypeContinent;
  uint32_t m_TypeCountry;
  uint32_t m_TypeCity;
  uint32_t m_TypeCityCapital;
  uint32_t m_TypeTown;
  uint32_t m_TypeVillage;
};

}  // unnamed namespace

m2::RectD feature::GetFeatureViewport(FeatureType const & feature)
{
  static FeatureViewportEstimator const featureViewportEstimator;
  return featureViewportEstimator.GetViewport(feature);
}
