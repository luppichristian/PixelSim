VertexOutput fullscreen_vertex(uint id : SV_VertexID)
{
  VertexOutput output;
  output.uv = float2((id << 1) & 2, id & 2);
  output.position =
      float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return output;
}

TextVertexOutput text_vertex(TextVertexInput input)
{
  TextVertexOutput output;
  output.position = float4(input.position, 0.0, 1.0);
  output.uv = input.uv;
  output.color = input.color;
  return output;
}

float4 draw_text(TextVertexOutput input) : SV_Target
{
  float coverage = FontAtlas.Sample(PointSampler, input.uv);
  return float4(input.color.rgb, input.color.a * coverage);
}

float4 pixel_color(uint pixel)
{
  bool containsDissolvedSalt =
      pixel_type(pixel) == WATER && (pixel & DISSOLVED_SALT_MASK) != 0u;
  pixel = pixel_type(pixel);
  if (pixel == WOOD)
    return float4(0.38, 0.20, 0.07, 1.0);
  if (pixel == IRON)
    return float4(0.38, 0.42, 0.45, 1.0);
  if (pixel == SAND)
    return float4(0.82, 0.67, 0.30, 1.0);
  if (pixel == RUST)
    return float4(0.56, 0.18, 0.05, 1.0);
  if (pixel == WATER)
    return containsDissolvedSalt ? float4(0.20, 0.48, 0.84, 1.0)
                                 : float4(0.05, 0.34, 0.82, 1.0);
  if (pixel == LAVA)
    return float4(1.0, 0.20, 0.01, 1.0);
  if (pixel == SMOKE)
    return float4(0.22, 0.23, 0.25, 1.0);
  if (pixel == STEAM)
    return float4(0.78, 0.84, 0.88, 1.0);
  if (pixel == ACID)
    return float4(0.18, 0.95, 0.16, 1.0);
  if (pixel == FIRE)
    return float4(1.0, 0.58, 0.04, 1.0);
  if (pixel == OIL)
    return float4(0.20, 0.15, 0.05, 1.0);
  if (pixel == SALT)
    return float4(0.92, 0.90, 0.82, 1.0);
  if (pixel == BEDROCK)
    return float4(0.16, 0.17, 0.20, 1.0);
  return float4(0.025, 0.03, 0.045, 1.0);
}

float4 draw_pixels(VertexOutput input) : SV_Target
{
  uint2 viewportPixel = uint2(input.position.xy) - ViewportOrigin;
  uint2 cell =
      min(viewportPixel * SimulationSize / ViewportSize, SimulationSize - 1u);
  float4 color = pixel_color(StateIn[cell]);

  // D3D11 owns this surface, so draw the clickable material palette in the same
  // pass.
  int2 palettePosition = int2(cell) - int2(8, 6);
  int paletteRow = palettePosition.y / 28;
  int paletteColumn = palettePosition.x / 28;
  int button = paletteRow * 14 + paletteColumn;
  int localX = palettePosition.x - paletteColumn * 28;
  int localY = palettePosition.y - paletteRow * 28;
  bool insidePalette = palettePosition.x >= 0 && palettePosition.y >= 0 &&
                       paletteColumn < 14 && localY < 24 &&
                       button >= int(EMPTY) && button <= int(BEDROCK) &&
                       localX < 24;
  if (insidePalette)
  {
    uint material = uint(button);
    float4 swatch = pixel_color(material);
    if (material == EMPTY)
    {
      bool eraseMark = abs(localX - localY) <= 1 ||
                       abs((23 - localX) - localY) <= 1;
      swatch = eraseMark ? float4(0.78, 0.80, 0.84, 1.0)
                         : float4(0.10, 0.11, 0.13, 1.0);
    }
    bool selected = material == SelectedPixel;
    bool border = localX < 2 || localX >= 22 || localY < 2 || localY >= 22;
    color = selected && border ? float4(1.0, 1.0, 1.0, 1.0) : swatch;
  }
  return color;
}

float4 draw_bloom_emission(VertexOutput input) : SV_Target
{
  uint2 cell = min(uint2(input.uv * SimulationSize), SimulationSize - 1u);
  uint material = pixel_type(StateIn[cell]);
  if (material == FIRE)
    return float4(pixel_color(material).rgb * 1.4, 1.0);
  if (material == LAVA)
    return float4(pixel_color(material).rgb * 1.2, 1.0);
  if (material == ACID)
    return float4(pixel_color(material).rgb * 0.9, 1.0);
  return float4(0.0, 0.0, 0.0, 1.0);
}

float4 blur_bloom(float2 uv, float2 direction)
{
  uint width;
  uint height;
  PostSource.GetDimensions(width, height);
  float2 texel = direction / float2(width, height);
  float3 color = PostSource.Sample(LinearSampler, uv).rgb * 0.227027;
  color += PostSource.Sample(LinearSampler, uv + texel * 1.384615).rgb * 0.316216;
  color += PostSource.Sample(LinearSampler, uv - texel * 1.384615).rgb * 0.316216;
  color += PostSource.Sample(LinearSampler, uv + texel * 3.230769).rgb * 0.070270;
  color += PostSource.Sample(LinearSampler, uv - texel * 3.230769).rgb * 0.070270;
  return float4(color, 1.0);
}

float4 blur_bloom_horizontal(VertexOutput input) : SV_Target
{
  return blur_bloom(input.uv, float2(1.0, 0.0));
}

float4 blur_bloom_vertical(VertexOutput input) : SV_Target
{
  return blur_bloom(input.uv, float2(0.0, 1.0));
}

float4 composite_bloom(VertexOutput input) : SV_Target
{
  float3 scene = PostSource.Sample(LinearSampler, input.uv).rgb;
  float3 bloom = BloomSource.Sample(LinearSampler, input.uv).rgb;
  return float4(saturate(scene + bloom * 0.75), 1.0);
}
