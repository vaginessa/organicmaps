#include "drape_frontend/line_shape.hpp"

#include "drape_frontend/line_shape_helper.hpp"

#include "shaders/programs.hpp"

#include "drape/attribute_provider.hpp"
#include "drape/batcher.hpp"
#include "drape/glsl_types.hpp"
#include "drape/glsl_func.hpp"
#include "drape/support_manager.hpp"
#include "drape/texture_manager.hpp"
#include "drape/utils/vertex_decl.hpp"

#include "indexer/scales.hpp"

#include "base/logging.hpp"

#include <algorithm>
#include <vector>

namespace df
{
namespace
{
class TextureCoordGenerator
{
public:
  explicit TextureCoordGenerator(dp::TextureManager::StippleRegion const & region)
    : m_region(region), m_maskSize(m_region.GetMaskPixelSize())
  {}

  glsl::vec4 GetTexCoordsByDistance(float distance, bool isLeft) const
  {
    m2::RectF const & texRect = m_region.GetTexRect();
    return { distance / GetMaskLength(), texRect.minX(), texRect.SizeX(), isLeft ? texRect.minY() : texRect.maxY() };
  }

  uint32_t GetMaskLength() const
  {
    return m_maskSize.x;
  }

  dp::TextureManager::StippleRegion const & GetRegion() const
  {
    return m_region;
  }

private:
  dp::TextureManager::StippleRegion const m_region;
  m2::PointU const m_maskSize;
};

struct BaseBuilderParams
{
  dp::TextureManager::ColorRegion m_color;
  dp::TextureManager::ColorRegion m_capColor;
  dp::TextureManager::ColorRegion m_joinColor;
  float m_pxHalfWidth;
  float m_depth;
  bool m_depthTestEnabled;
  DepthLayer m_depthLayer;
  dp::LineCap m_cap;
  dp::LineJoin m_join;
};

template <typename TVertex>
class BaseLineBuilder : public LineShapeInfo
{
public:
  BaseLineBuilder(BaseBuilderParams const & params, size_t geomsSize, size_t joinsSize)
    : m_params(params)
    , m_colorCoord(glsl::ToVec2(params.m_color.GetTexRect().Center()))
    , m_capColorCoord(glsl::ToVec2(params.m_capColor.GetTexRect().Center()))
    , m_joinColorCoord(glsl::ToVec2(params.m_joinColor.GetTexRect().Center()))
  {
    m_geometry.reserve(geomsSize);
    m_joinGeom.reserve(joinsSize);
    m_params.m_cap = dp::LineCap::RoundCap; //pastk
  }

  dp::BindingInfo const & GetBindingInfo() override
  {
    return TVertex::GetBindingInfo();
  }

  ref_ptr<void> GetLineData() override
  {
    return make_ref(m_geometry.data());
  }

  uint32_t GetLineSize() override
  {
    return static_cast<uint32_t>(m_geometry.size());
  }
  /*
  ref_ptr<void> GetJoinData() override
  {
    return make_ref(m_joinGeom.data());
  }

  uint32_t GetJoinSize() override
  {
    return static_cast<uint32_t>(m_joinGeom.size());
  }
  */

  float GetHalfWidth()
  {
    return m_params.m_pxHalfWidth;
  }

  dp::BindingInfo const & GetCapBindingInfo() override
  {
    return GetBindingInfo();
  }

  dp::RenderState GetCapState() override
  {
    return GetState();
  }

  dp::RenderState GetJoinState() override
  {
    return GetState();
  }

  ref_ptr<void> GetCapData() override
  {
    return ref_ptr<void>();
  }

  uint32_t GetCapSize() override
  {
    return 0;
  }

  ref_ptr<void> GetJoinData() override
  {
    return ref_ptr<void>();
  }

  uint32_t GetJoinSize() override
  {
    return 0;
  }

  float GetSide(bool isLeft) const
  {
    return isLeft ? 1.0f : -1.0f;
  }

protected:
  using V = TVertex;
  using TGeometryBuffer = gpu::VBReservedSizeT<V>;

  TGeometryBuffer m_geometry;
  TGeometryBuffer m_joinGeom; //pastk not used??

  BaseBuilderParams m_params;
  glsl::vec2 const m_colorCoord;
  glsl::vec2 const m_capColorCoord; //pastk
  glsl::vec2 const m_joinColorCoord;
};

class SolidLineBuilder : public BaseLineBuilder<gpu::LineVertex>
{
  using TBase = BaseLineBuilder<gpu::LineVertex>;
  using TNormal = gpu::LineVertex::TNormal;

  struct CapVertex
  {
    using TPosition = gpu::LineVertex::TPosition;
    using TNormal = gpu::LineVertex::TNormal;
    using TTexCoord = gpu::LineVertex::TTexCoord;

    CapVertex() {}
    CapVertex(TPosition const & pos, TNormal const & normal, TTexCoord const & color)
      : m_position(pos)
      , m_normal(normal)
      , m_color(color)
    {}

    TPosition m_position;
    TNormal m_normal;
    TTexCoord m_color;
  };

  using TCapBuffer = gpu::VBUnknownSizeT<CapVertex>;

public:
  using BuilderParams = BaseBuilderParams;

  SolidLineBuilder(BuilderParams const & params, size_t pointsInSpline)
    : TBase(params, pointsInSpline * 2, (pointsInSpline - 2) * 8)
  {}

  dp::RenderState GetState() override
  {
    auto state = CreateRenderState(gpu::Program::Line, m_params.m_depthLayer);
    state.SetColorTexture(m_params.m_capColor.GetTexture()); //pastk
    state.SetDepthTestEnabled(m_params.m_depthTestEnabled);
    return state;
  }

  dp::BindingInfo const & GetCapBindingInfo() override
  {
    if (m_params.m_cap == dp::ButtCap)
      return TBase::GetCapBindingInfo();

    static std::unique_ptr<dp::BindingInfo> s_capInfo;
    if (s_capInfo == nullptr)
    {
      dp::BindingFiller<CapVertex> filler(3);
      filler.FillDecl<CapVertex::TPosition>("a_position");
      filler.FillDecl<CapVertex::TNormal>("a_normal");
      filler.FillDecl<CapVertex::TTexCoord>("a_colorTexCoords");

      s_capInfo.reset(new dp::BindingInfo(filler.m_info));
    }

    return *s_capInfo;
  }

  dp::RenderState GetCapState() override
  {
    if (m_params.m_cap == dp::ButtCap)
      return TBase::GetCapState();

    auto state = CreateRenderState(gpu::Program::CapJoin, m_params.m_depthLayer);
    state.SetDepthTestEnabled(m_params.m_depthTestEnabled);
    state.SetColorTexture(m_params.m_capColor.GetTexture()); //pastk
    state.SetDepthFunction(dp::TestFunction::Less);
    return state;
  }

  dp::RenderState GetJoinState() override
  {
    if (m_params.m_cap == dp::ButtCap)
      return TBase::GetCapState();

    auto state = CreateRenderState(gpu::Program::CapJoin, m_params.m_depthLayer);
    state.SetDepthTestEnabled(m_params.m_depthTestEnabled);
    state.SetColorTexture(m_params.m_joinColor.GetTexture()); //pastk
    state.SetDepthFunction(dp::TestFunction::Less);
    return state;
  }

  ref_ptr<void> GetCapData() override
  {
    return make_ref<void>(m_capGeometry.data());
  }

  uint32_t GetCapSize() override
  {
    return static_cast<uint32_t>(m_capGeometry.size());
  }

  ref_ptr<void> GetJoinData() override
  {
    return make_ref<void>(m_jGeometry.data());
  }

  uint32_t GetJoinSize() override
  {
    return static_cast<uint32_t>(m_jGeometry.size());
  }

  void SubmitVertex(glsl::vec3 const & pivot, glsl::vec2 const & normal, bool isLeft)
  {
    float const halfWidth = GetHalfWidth();
    m_geometry.emplace_back(pivot, TNormal(halfWidth * normal, halfWidth * GetSide(isLeft)), m_colorCoord);
  }

  void SubmitJoin(glsl::vec2 const & pos)
  {
    //if (m_params.m_join == dp::RoundJoin)
      CreateRoundCap(pos, false);
  }

  void SubmitCap(glsl::vec2 const & pos)
  {
    //if (m_params.m_cap != dp::ButtCap) /pastk
      CreateRoundCap(pos, true);
  }

private:
  void CreateRoundCap(glsl::vec2 const & pos, bool isCap)
  {
    // Here we use an equilateral triangle to render circle (incircle of a triangle).
    static float const kSqrt3 = sqrt(3.0f);
    float const size = isCap ? 2.0f : 1.6f;
    float const radius = GetHalfWidth();
    auto const color = isCap ? m_capColorCoord : m_joinColorCoord;
    auto bucket = isCap ? &m_capGeometry : &m_jGeometry;
    float const depth = isCap ? dp::kMaxDepth : dp::kMaxDepth - 1;

    bucket->emplace_back(CapVertex::TPosition(pos, depth),
                               CapVertex::TNormal(-radius * kSqrt3 * size, -radius * size, radius * size),
                               CapVertex::TTexCoord(color));
    bucket->emplace_back(CapVertex::TPosition(pos, depth),
                               CapVertex::TNormal(radius * kSqrt3 * size, -radius * size, radius * size),
                               CapVertex::TTexCoord(color));
    bucket->emplace_back(CapVertex::TPosition(pos, depth),
                               CapVertex::TNormal(0, 2.0f * radius * size, radius * size),
                               CapVertex::TTexCoord(color));
  }

private:
  TCapBuffer m_capGeometry;
  TCapBuffer m_jGeometry;
};

class SimpleSolidLineBuilder : public BaseLineBuilder<gpu::AreaVertex>
{
  using TBase = BaseLineBuilder<gpu::AreaVertex>;

public:
  using BuilderParams = BaseBuilderParams;

  SimpleSolidLineBuilder(BuilderParams const & params, size_t pointsInSpline, int lineWidth)
    : TBase(params, pointsInSpline, 0)
    , m_lineWidth(lineWidth)
  {}

  dp::RenderState GetState() override
  {
    auto state = CreateRenderState(gpu::Program::AreaOutline, m_params.m_depthLayer);
    state.SetDepthTestEnabled(m_params.m_depthTestEnabled);
    state.SetColorTexture(m_params.m_color.GetTexture());
    state.SetDrawAsLine(true);
    state.SetLineWidth(m_lineWidth);
    return state;
  }

  void SubmitVertex(glsl::vec3 const & pivot)
  {
    m_geometry.emplace_back(pivot, m_colorCoord);
  }

private:
  int m_lineWidth;
};

class DashedLineBuilder : public BaseLineBuilder<gpu::DashedLineVertex>
{
  using TBase = BaseLineBuilder<gpu::DashedLineVertex>;
  using TNormal = gpu::LineVertex::TNormal;

public:
  struct BuilderParams : BaseBuilderParams
  {
    dp::TextureManager::StippleRegion m_stipple;
    float m_glbHalfWidth;
    float m_baseGtoP;
  };

  DashedLineBuilder(BuilderParams const & params, size_t pointsInSpline)
    : TBase(params, pointsInSpline * 8, (pointsInSpline - 2) * 8)
    , m_texCoordGen(params.m_stipple)
    , m_baseGtoPScale(params.m_baseGtoP)
  {}

  float GetMaskLengthG() const
  {
    return m_texCoordGen.GetMaskLength() / m_baseGtoPScale;
  }

  dp::RenderState GetState() override
  {
    auto state = CreateRenderState(gpu::Program::DashedLine, m_params.m_depthLayer);
    state.SetDepthTestEnabled(m_params.m_depthTestEnabled);
    state.SetColorTexture(m_params.m_color.GetTexture());
    state.SetMaskTexture(m_texCoordGen.GetRegion().GetTexture());
    return state;
  }

  void SubmitVertex(glsl::vec3 const & pivot, glsl::vec2 const & normal, bool isLeft, float offsetFromStart)
  {
    float const halfWidth = GetHalfWidth();
    m_geometry.emplace_back(pivot, TNormal(halfWidth * normal, halfWidth * GetSide(isLeft)),
                            m_colorCoord, m_texCoordGen.GetTexCoordsByDistance(offsetFromStart, isLeft));
  }

private:
  TextureCoordGenerator m_texCoordGen;
  float const m_baseGtoPScale;
};
}  // namespace

LineShape::LineShape(m2::SharedSpline const & spline, LineViewParams const & params)
  : m_params(params)
  , m_spline(spline)
  , m_isSimple(false)
{
  ASSERT_GREATER(m_spline->GetPath().size(), 1, ());
}

template <typename TBuilder>
void LineShape::Construct(TBuilder & builder) const
{
  ASSERT(false, ("No implementation"));
}

// Specialization optimized for dashed lines.
template <>
void LineShape::Construct<DashedLineBuilder>(DashedLineBuilder & builder) const
{
  std::vector<m2::PointD> const & path = m_spline->GetPath();
  ASSERT_GREATER(path.size(), 1, ());

  float const toShapeFactor = kShapeCoordScalar;  // the same as in ToShapeVertex2

  // Each segment should lie in pattern mask according to the "longest" possible pixel length in current tile.
  // Since, we calculate vertices once, usually for the "smallest" tile scale, need to apply divide factor here.
  // In other words, if m_baseGtoPScale = Scale(tileLevel), we should use Scale(tileLevel + 1) to calculate 'maskLengthG'.
  /// @todo Logically, the factor should be 2, but drawing artifacts are still present at higher visual scales.
  /// Use 3 for the best quality, but need to review here, probably I missed something.
  float const maskLengthG = builder.GetMaskLengthG() / 3;

  float offset = 0;
  for (size_t i = 1; i < path.size(); ++i)
  {
    if (path[i].EqualDxDy(path[i - 1], kMwmPointAccuracy))
      continue;

    glsl::vec2 const p1 = ToShapeVertex2(path[i - 1]);
    glsl::vec2 const p2 = ToShapeVertex2(path[i]);
    glsl::vec2 tangent, leftNormal, rightNormal;
    CalculateTangentAndNormals(p1, p2, tangent, leftNormal, rightNormal);

    float toDraw = path[i].Length(path[i - 1]);

    glsl::vec2 currPivot = p1;
    do
    {
      glsl::vec2 nextPivot;
      float nextOffset = offset + toDraw;
      if (maskLengthG >= nextOffset)
      {
        // Fast lane, where most of segments, that fit into mask, should draw.
        nextPivot = p2;
        toDraw = 0;
      }
      else
      {
        // Break path section here.
        float const len = maskLengthG - offset;
        ASSERT_GREATER(len, 0, ());
        nextPivot = currPivot + tangent * (len * toShapeFactor);

        nextOffset = maskLengthG;
        toDraw -= len;
      }

      builder.SubmitVertex({currPivot, m_params.m_depth}, rightNormal, false /* isLeft */, offset);
      builder.SubmitVertex({currPivot, m_params.m_depth}, leftNormal, true /* isLeft */, offset);
      builder.SubmitVertex({nextPivot, m_params.m_depth}, rightNormal, false /* isLeft */, nextOffset);
      builder.SubmitVertex({nextPivot, m_params.m_depth}, leftNormal, true /* isLeft */, nextOffset);

      currPivot = nextPivot;
      offset = nextOffset;
      if (offset >= maskLengthG)
        offset = 0;

    } while (toDraw > 0);
  }
}

// Specialization optimized for solid lines.
template <>
void LineShape::Construct<SolidLineBuilder>(SolidLineBuilder & builder) const
{
  std::vector<m2::PointD> const & path = m_spline->GetPath();
  ASSERT_GREATER(path.size(), 1, ());

  // skip joins generation
  float const kJoinsGenerationThreshold = 2.5f;
  bool generateJoins = true;
  //if (builder.GetHalfWidth() <= kJoinsGenerationThreshold) //pastk
  //  generateJoins = false;

  // build geometry
  glsl::vec2 firstPoint = ToShapeVertex2(path.front());
  glsl::vec2 lastPoint;
  bool hasConstructedSegments = false;
  for (size_t i = 1; i < path.size(); ++i)
  {
    if (path[i].EqualDxDy(path[i - 1], 1.0E-5))
      continue;

    glsl::vec2 const p1 = ToShapeVertex2(path[i - 1]);
    glsl::vec2 const p2 = ToShapeVertex2(path[i]);
    glsl::vec2 tangent, leftNormal, rightNormal;
    CalculateTangentAndNormals(p1, p2, tangent, leftNormal, rightNormal);

    glsl::vec3 const startPoint = glsl::vec3(p1, m_params.m_depth);
    glsl::vec3 const endPoint = glsl::vec3(p2, m_params.m_depth);

    builder.SubmitVertex(startPoint, rightNormal, false /* isLeft */);
    builder.SubmitVertex(startPoint, leftNormal, true /* isLeft */);
    builder.SubmitVertex(endPoint, rightNormal, false /* isLeft */);
    builder.SubmitVertex(endPoint, leftNormal, true /* isLeft */);

    // generate joins
    if (generateJoins && i < path.size() - 1)
      builder.SubmitJoin(p2);

    lastPoint = p2;
    hasConstructedSegments = true;
  }

  if (hasConstructedSegments)
  {
    builder.SubmitCap(firstPoint);
    builder.SubmitCap(lastPoint);
  }
}

// Specialization optimized for simple solid lines.
template <>
void LineShape::Construct<SimpleSolidLineBuilder>(SimpleSolidLineBuilder & builder) const
{
  std::vector<m2::PointD> const & path = m_spline->GetPath();
  ASSERT_GREATER(path.size(), 1, ());

  // Build geometry.
  for (m2::PointD const & pt : path)
    builder.SubmitVertex(glsl::vec3(ToShapeVertex2(pt), m_params.m_depth));
}

bool LineShape::CanBeSimplified(int & lineWidth) const
{
  // Disable simplification for world map.
  if (m_params.m_zoomLevel > 0 && m_params.m_zoomLevel <= scales::GetUpperCountryScale())
    return false;

  static float width = std::min(2.5f, dp::SupportManager::Instance().GetMaxLineWidth());
  if (m_params.m_width <= width)
  {
    lineWidth = std::max(1, static_cast<int>(m_params.m_width));
    return true;
  }

  lineWidth = 1;
  return false;
}

void LineShape::Prepare(ref_ptr<dp::TextureManager> textures) const
{
  auto commonParamsBuilder = [this, textures](BaseBuilderParams & p)
  {
    dp::TextureManager::ColorRegion colorRegion;
    textures->GetColorRegion(m_params.m_color, colorRegion);

    p.m_cap = m_params.m_cap;
    p.m_color = colorRegion;
    p.m_depthTestEnabled = m_params.m_depthTestEnabled;
    p.m_depth = m_params.m_depth;
    p.m_depthLayer = m_params.m_depthLayer;
    p.m_join = dp::LineJoin::RoundJoin; //m_params.m_join; pastk
    p.m_pxHalfWidth = 1; //m_params.m_width / 2; pastk

    dp::TextureManager::ColorRegion capColorRegion;
    textures->GetColorRegion(dp::Color::Red(), capColorRegion);
    p.m_capColor = capColorRegion;

    dp::TextureManager::ColorRegion joinColorRegion;
    textures->GetColorRegion(dp::Color::Blue(), joinColorRegion);
    p.m_joinColor = joinColorRegion;
  };

  if (m_params.m_pattern.empty())
  {
    int lineWidth = 1;
    m_isSimple = CanBeSimplified(lineWidth);
    m_isSimple = false; //pastk
    if (m_isSimple)
    {
      // Uses GL lines primitives for rendering.
      SimpleSolidLineBuilder::BuilderParams p;
      commonParamsBuilder(p);

      auto builder = std::make_unique<SimpleSolidLineBuilder>(p, m_spline->GetPath().size(), lineWidth);
      Construct<SimpleSolidLineBuilder>(*builder);
      m_lineShapeInfo = std::move(builder);
    }
    else
    {
      // Expands lines to quads on CPU side.
      SolidLineBuilder::BuilderParams p;
      commonParamsBuilder(p);

      auto builder = std::make_unique<SolidLineBuilder>(p, m_spline->GetPath().size());
      Construct<SolidLineBuilder>(*builder);
      m_lineShapeInfo = std::move(builder);
    }
  }
  else
  {
    dp::TextureManager::StippleRegion maskRegion;
    textures->GetStippleRegion(m_params.m_pattern, maskRegion);

    DashedLineBuilder::BuilderParams p;
    commonParamsBuilder(p);
    p.m_stipple = maskRegion;
    p.m_baseGtoP = m_params.m_baseGtoPScale;
    p.m_glbHalfWidth = p.m_pxHalfWidth / m_params.m_baseGtoPScale;

    auto builder = std::make_unique<DashedLineBuilder>(p, m_spline->GetPath().size());
    Construct<DashedLineBuilder>(*builder);
    m_lineShapeInfo = std::move(builder);
  }
}

void LineShape::Draw(ref_ptr<dp::GraphicsContext> context, ref_ptr<dp::Batcher> batcher,
                     ref_ptr<dp::TextureManager> textures) const
{
  if (!m_lineShapeInfo)
    Prepare(textures);

  ASSERT(m_lineShapeInfo != nullptr, ());
  dp::RenderState state = m_lineShapeInfo->GetState();
  dp::AttributeProvider provider(1, m_lineShapeInfo->GetLineSize());
  provider.InitStream(0, m_lineShapeInfo->GetBindingInfo(), m_lineShapeInfo->GetLineData());
  if (!m_isSimple)
  {
    batcher->InsertListOfStrip(context, state, make_ref(&provider), dp::Batcher::VertexPerQuad);

    /*
    uint32_t const joinSize = m_lineShapeInfo->GetJoinSize(); //pastk not used
    if (joinSize > 0)
    {
      dp::AttributeProvider joinsProvider(1, joinSize);
      joinsProvider.InitStream(0, m_lineShapeInfo->GetBindingInfo(), m_lineShapeInfo->GetJoinData());
      batcher->InsertTriangleList(context, state, make_ref(&joinsProvider));
    }
    */

    uint32_t const joinSize = m_lineShapeInfo->GetJoinSize();
    if (joinSize > 0)
    {
      dp::AttributeProvider joinProvider(1, joinSize);
      joinProvider.InitStream(0, m_lineShapeInfo->GetCapBindingInfo(), m_lineShapeInfo->GetJoinData());
      batcher->InsertTriangleList(context, m_lineShapeInfo->GetJoinState(), make_ref(&joinProvider));
    }

    uint32_t const capSize = m_lineShapeInfo->GetCapSize();
    if (capSize > 0)
    {
      dp::AttributeProvider capProvider(1, capSize);
      capProvider.InitStream(0, m_lineShapeInfo->GetCapBindingInfo(), m_lineShapeInfo->GetCapData());
      batcher->InsertTriangleList(context, m_lineShapeInfo->GetCapState(), make_ref(&capProvider));
    }
  }
  else
  {
    batcher->InsertLineStrip(context, state, make_ref(&provider));
  }
}
}  // namespace df

