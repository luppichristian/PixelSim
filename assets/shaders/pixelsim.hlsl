#define EMPTY 0u
#define WOOD 1u
#define IRON 2u
#define SAND 3u
#define RUST 4u
#define WATER 5u
#define LAVA 6u
#define SMOKE 7u
#define STEAM 8u
#define TYPE_MASK 0xffu
#define VELOCITY_Y_SHIFT 16u
#define MAX_FALL_SPEED 8
#define ROW_CAPACITY 640u
#define ROW_THREADS 64u


struct BrushCommand
{
  uint type;
  int x;
  int y;
  int radius;
  uint seed;
};

Texture2D<uint> StateIn : register(t0);
StructuredBuffer<BrushCommand> Brush : register(t1);
RWTexture2D<uint> StateOut : register(u0);
SamplerState PointSampler : register(s0);

cbuffer Frame : register(b0)
{
  uint FrameIndex;
  uint2 SimulationSize;
  uint SelectedPixel;
  uint2 ViewportOrigin;
  uint2 ViewportSize;
  uint2 BrushOrigin;
  uint2 BrushExtent;
  uint HorizontalPhase;
  uint3 FramePadding;
};

struct VertexOutput
{
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
};

uint Hash(uint2 p, uint salt)
{
  uint h = p.x * 0x8da6b343u + p.y * 0xd8163841u + salt * 0xcb1ab31fu;
  h ^= h >> 13;
  h *= 0x85ebca6bu;
  return h ^ (h >> 16);
}

uint PixelType(uint pixel) { return pixel & TYPE_MASK; }
bool IsSolid(uint p) { p = PixelType(p); return p == WOOD || p == IRON; }
bool IsPowder(uint p) { p = PixelType(p); return p == SAND || p == RUST; }
bool IsLiquid(uint p) { p = PixelType(p); return p == WATER || p == LAVA; }
bool IsGas(uint p) { p = PixelType(p); return p == SMOKE || p == STEAM; }
bool IsOpen(uint p) { return PixelType(p) == EMPTY || IsGas(p); }

uint VelocityY(uint pixel)
{
  return (pixel >> VELOCITY_Y_SHIFT) & 0xffu;
}

uint WithVelocityY(uint pixel, uint velocity)
{
  return (pixel & ~(0xffu << VELOCITY_Y_SHIFT)) |
         ((velocity & 0xffu) << VELOCITY_Y_SHIFT);
}

uint Read(int2 p)
{
  if (p.x < 0 || p.y < 0 || p.x >= int(SimulationSize.x) ||
      p.y >= int(SimulationSize.y))
    return IRON;
  return StateIn[p];
}

bool CanFallInto(uint mover, uint resident)
{
  mover = PixelType(mover);
  resident = PixelType(resident);
  if (resident == EMPTY || IsGas(resident))
    return true;
  if ((mover == SAND || mover == RUST) && resident == WATER)
    return true;
  if (mover == RUST && resident == LAVA)
    return true;
  return false;
}

bool IsStableSupport(uint liquid, int2 position)
{
  int2 supportPosition = position + int2(0, 1);
  uint support = Read(supportPosition);
  if (IsSolid(support))
    return true;
  if (IsLiquid(support))
  {
    // Two airborne liquid cells still fall as a column. A third liquid cell or
    // stable material below them supplies local pressure, allowing deep piles
    // to spread instead of behaving like rigid powder.
    return PixelType(support) == PixelType(liquid) &&
           !CanFallInto(support, Read(supportPosition + int2(0, 1)));
  }
  if (!IsPowder(support))
    return false;

  // A powder is support only when it cannot fall or roll away next.
  return !CanFallInto(support, Read(supportPosition + int2(0, 1))) &&
         !CanFallInto(support, Read(supportPosition + int2(-1, 1))) &&
         !CanFallInto(support, Read(supportPosition + int2(1, 1)));
}

bool IsSupported(uint liquid, int2 position)
{
  return IsLiquid(liquid) && IsStableSupport(liquid, position);
}

void SwapPixels(inout uint first, inout uint second)
{
  uint temporary = first;
  first = second;
  second = temporary;
}

void ReactPair(inout uint first, inout uint second, uint random)
{
  uint firstType = PixelType(first);
  uint secondType = PixelType(second);
  if ((firstType == WATER && secondType == LAVA) ||
      (firstType == LAVA && secondType == WATER))
  {
    if (firstType == WATER)
    {
      first = STEAM;
      second = IRON;
    }
    else
    {
      first = IRON;
      second = STEAM;
    }
  }
  else if ((random & 7u) == 0u && firstType == WOOD && secondType == LAVA)
  {
    first = SMOKE;
  }
  else if ((random & 7u) == 0u && firstType == LAVA && secondType == WOOD)
  {
    second = SMOKE;
  }
  else if ((random & 63u) == 0u && firstType == RUST && secondType == IRON)
  {
    second = RUST;
  }
  else if ((random & 63u) == 0u && firstType == IRON && secondType == RUST)
  {
    first = RUST;
  }
}

void ReactBlock(inout uint a, inout uint b, inout uint c, inout uint d,
                uint random)
{
  // Rotate edge priority so dense interfaces have no fixed favored edge.
  uint order = random & 3u;
  if (order == 0u)
  {
    ReactPair(a, b, random >> 2);
    ReactPair(a, c, random >> 5);
    ReactPair(b, d, random >> 8);
    ReactPair(c, d, random >> 11);
  }
  else if (order == 1u)
  {
    ReactPair(c, d, random >> 11);
    ReactPair(b, d, random >> 8);
    ReactPair(a, c, random >> 5);
    ReactPair(a, b, random >> 2);
  }
  else if (order == 2u)
  {
    ReactPair(a, c, random >> 5);
    ReactPair(c, d, random >> 11);
    ReactPair(a, b, random >> 2);
    ReactPair(b, d, random >> 8);
  }
  else
  {
    ReactPair(b, d, random >> 8);
    ReactPair(a, b, random >> 2);
    ReactPair(c, d, random >> 11);
    ReactPair(a, c, random >> 5);
  }
}

void RisePair(inout uint top, inout uint bottom)
{
  if (IsGas(bottom) && PixelType(top) == EMPTY)
    SwapPixels(top, bottom);
}


[numthreads(8, 8, 1)] void ApplyBrush(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= BrushExtent.x || id.y >= BrushExtent.y)
    return;
  const BrushCommand command = Brush[0];
  uint2 position = BrushOrigin + id.xy;
  int2 delta = int2(position) - int2(command.x, command.y);
  bool inside = dot(delta, delta) <= command.radius * command.radius;
  if (inside)
    StateOut[position] = command.type;
}

    [numthreads(8, 8, 1)] void Simulate(uint3 id : SV_DispatchThreadID)
{
  uint2 blockCount = (SimulationSize + 2u) / 2u;

  if (id.x >= blockCount.x || id.y >= blockCount.y)
    return;

  // Four phases vary X and Y ownership independently. A stationary cell
  // therefore gets access to both diagonals instead of remaining parity-locked.
  uint phase = FrameIndex & 3u;
  // Y alternates every update so a falling cell can keep moving each pass;
  // X alternates every two updates to break diagonal and lateral parity locks.
  int2 offset = int2(int((phase >> 1u) & 1u), int(phase & 1u));
  int2 origin = int2(id.xy) * 2 + offset - 1;
  int2 topLeft = origin;
  int2 topRight = origin + int2(1, 0);
  int2 bottomLeft = origin + int2(0, 1);
  int2 bottomRight = origin + int2(1, 1);

  uint a = Read(topLeft);
  uint b = Read(topRight);
  uint c = Read(bottomLeft);
  uint d = Read(bottomRight);
  uint random = Hash(id.xy, FrameIndex);

  ReactBlock(a, b, c, d, random);

  // Four non-overlapping 2x2 phases give every output cell one writer while
  // allowing particles to cross each block edge on a later update.
  // The column-owned pass handles direct accelerated falling. This local
  // pass only rolls powder diagonally when the cell directly below is blocked.
  if (IsPowder(a) && !CanFallInto(a, c) && CanFallInto(a, d))
    SwapPixels(a, d);
  if (IsPowder(b) && !CanFallInto(b, d) && CanFallInto(b, c))
    SwapPixels(b, c);

  RisePair(a, c);
  RisePair(b, d);

  // Gases diffuse horizontally without creating or deleting cells.
  if ((random & 2u) != 0u)
  {
    if (IsGas(a) && PixelType(b) == EMPTY)
      SwapPixels(a, b);
    if (IsGas(c) && PixelType(d) == EMPTY)
      SwapPixels(c, d);
  }
  else
  {
    if (PixelType(a) == EMPTY && IsGas(b))
      SwapPixels(a, b);
    if (PixelType(c) == EMPTY && IsGas(d))
      SwapPixels(c, d);
  }

  if (PixelType(a) == STEAM && (random & 1023u) == 0u)
    a = WATER;
  if (PixelType(b) == STEAM && ((random >> 2) & 1023u) == 0u)
    b = WATER;
  if (PixelType(c) == STEAM && ((random >> 4) & 1023u) == 0u)
    c = WATER;
  if (PixelType(d) == STEAM && ((random >> 6) & 1023u) == 0u)
    d = WATER;

  if (topLeft.x >= 0 && topLeft.y >= 0 && topLeft.x < int(SimulationSize.x) &&
      topLeft.y < int(SimulationSize.y))
    StateOut[topLeft] = a;
  if (topRight.x >= 0 && topRight.y >= 0 &&
      topRight.x < int(SimulationSize.x) && topRight.y < int(SimulationSize.y))
    StateOut[topRight] = b;
  if (bottomLeft.x >= 0 && bottomLeft.y >= 0 &&
      bottomLeft.x < int(SimulationSize.x) &&
      bottomLeft.y < int(SimulationSize.y))
    StateOut[bottomLeft] = c;
  if (bottomRight.x >= 0 && bottomRight.y >= 0 &&
      bottomRight.x < int(SimulationSize.x) &&
      bottomRight.y < int(SimulationSize.y))
    StateOut[bottomRight] = d;
}

[numthreads(64, 1, 1)]
void MoveFallingMaterialsVertical(uint3 id : SV_DispatchThreadID)
{
  uint x = id.x;
  if (x >= SimulationSize.x)
    return;

  for (uint copyY = 0u; copyY < SimulationSize.y; ++copyY)
    StateOut[uint2(x, copyY)] = StateIn[uint2(x, copyY)];

  for (int sourceY = int(SimulationSize.y) - 1; sourceY >= 0; --sourceY)
  {
    int2 source = int2(int(x), sourceY);
    uint particle = StateOut[source];
    if (!IsLiquid(particle) && !IsPowder(particle))
      continue;

    uint speed = min(VelocityY(particle) + 1u, uint(MAX_FALL_SPEED));
    int destinationY = sourceY;
    for (uint distance = 1u; distance <= speed; ++distance)
    {
      int targetY = sourceY + int(distance);
      if (targetY >= int(SimulationSize.y))
        break;
      if (!CanFallInto(particle, StateOut[int2(int(x), targetY)]))
        break;
      destinationY = targetY;
    }

    if (destinationY == sourceY)
    {
      StateOut[source] = WithVelocityY(particle, 0u);
      continue;
    }

    int2 destination = int2(int(x), destinationY);
    uint resident = StateOut[destination];
    StateOut[destination] = WithVelocityY(particle, speed);
    StateOut[source] = resident;
  }
}

groupshared uint RowState[ROW_CAPACITY];
groupshared uint SupportOne[ROW_CAPACITY];
groupshared uint SupportTwo[ROW_CAPACITY];
groupshared uint SegmentId[ROW_CAPACITY];
groupshared uint ScanScratch[ROW_CAPACITY];
groupshared uint SourceForward[ROW_CAPACITY];
groupshared uint SourceBackward[ROW_CAPACITY];
groupshared uint DestinationForward[ROW_CAPACITY];
groupshared uint DestinationBackward[ROW_CAPACITY];

bool IsSharedPressureSource(uint x, uint liquidType)
{
  uint pixel = RowState[x];
  return PixelType(pixel) == liquidType && VelocityY(pixel) == 0u &&
         PixelType(SupportOne[x]) == liquidType &&
         !CanFallInto(SupportOne[x], SupportTwo[x]);
}

bool IsSharedPressureDestination(uint x, uint liquidType)
{
  return IsOpen(RowState[x]) && PixelType(SupportOne[x]) != liquidType;
}

void TransferSharedLiquidPressure(uint lane, uint liquidType, bool scanRight)
{
  for (uint x = lane; x < SimulationSize.x; x += ROW_THREADS)
  {
    uint type = PixelType(RowState[x]);
    SegmentId[x] = type != liquidType && !IsOpen(RowState[x]) ? 1u : 0u;
  }
  GroupMemoryBarrierWithGroupSync();

  for (uint offset = 1u; offset < ROW_CAPACITY; offset <<= 1u)
  {
    for (uint x = lane; x < SimulationSize.x; x += ROW_THREADS)
      ScanScratch[x] = SegmentId[x] + (x >= offset ? SegmentId[x - offset] : 0u);
    GroupMemoryBarrierWithGroupSync();
    for (uint x = lane; x < SimulationSize.x; x += ROW_THREADS)
      SegmentId[x] = ScanScratch[x];
    GroupMemoryBarrierWithGroupSync();
  }

  for (uint segment = lane; segment < SimulationSize.x;
       segment += ROW_THREADS)
  {
    SourceForward[segment] = 0xffffffffu;
    SourceBackward[segment] = 0u;
    DestinationForward[segment] = 0xffffffffu;
    DestinationBackward[segment] = 0u;
  }
  GroupMemoryBarrierWithGroupSync();

  for (uint x = lane; x < SimulationSize.x; x += ROW_THREADS)
  {
    uint type = PixelType(RowState[x]);
    if (type == liquidType || IsOpen(RowState[x]))
    {
      uint segment = SegmentId[x];
      if (IsSharedPressureSource(x, liquidType))
      {
        InterlockedMin(SourceForward[segment], x);
        InterlockedMax(SourceBackward[segment], x + 1u);
      }
    }
  }
  GroupMemoryBarrierWithGroupSync();

  for (uint x = lane; x < SimulationSize.x; x += ROW_THREADS)
  {
    uint type = PixelType(RowState[x]);
    if ((type == liquidType || IsOpen(RowState[x])) &&
        IsSharedPressureDestination(x, liquidType))
    {
      uint segment = SegmentId[x];
      if (SourceForward[segment] != 0xffffffffu &&
          x > SourceForward[segment])
        InterlockedMin(DestinationForward[segment], x);
      if (SourceBackward[segment] != 0u &&
          x + 1u < SourceBackward[segment])
        InterlockedMax(DestinationBackward[segment], x + 1u);
    }
  }
  GroupMemoryBarrierWithGroupSync();

  for (uint x = lane; x < SimulationSize.x; x += ROW_THREADS)
  {
    uint type = PixelType(RowState[x]);
    bool traversable = type == liquidType || IsOpen(RowState[x]);
    bool segmentStart = false;
    if (traversable)
    {
      segmentStart = x == 0u;
      if (x > 0u)
        segmentStart = PixelType(RowState[x - 1u]) != liquidType &&
                       !IsOpen(RowState[x - 1u]);
    }
    if (!segmentStart)
      continue;

    uint segment = SegmentId[x];
    uint source = 0xffffffffu;
    uint destination = 0xffffffffu;
    if (scanRight && DestinationForward[segment] != 0xffffffffu)
    {
      source = SourceForward[segment];
      destination = DestinationForward[segment];
    }
    else if (!scanRight && DestinationBackward[segment] != 0u)
    {
      source = SourceBackward[segment] - 1u;
      destination = DestinationBackward[segment] - 1u;
    }
    else if (DestinationForward[segment] != 0xffffffffu)
    {
      source = SourceForward[segment];
      destination = DestinationForward[segment];
    }
    else if (DestinationBackward[segment] != 0u)
    {
      source = SourceBackward[segment] - 1u;
      destination = DestinationBackward[segment] - 1u;
    }

    if (destination != 0xffffffffu)
      SwapPixels(RowState[source], RowState[destination]);
  }
  GroupMemoryBarrierWithGroupSync();
}

[numthreads(ROW_THREADS, 1, 1)]
void MoveLiquidsHorizontal(uint3 groupId : SV_GroupID,
                           uint lane : SV_GroupIndex)
{
  uint y = HorizontalPhase + groupId.y * 3u;
  if (y >= SimulationSize.y)
    return;

  for (uint x = lane; x < SimulationSize.x; x += ROW_THREADS)
  {
    RowState[x] = StateOut[uint2(x, y)];
    SupportOne[x] = y + 1u < SimulationSize.y
                        ? StateOut[uint2(x, y + 1u)]
                        : IRON;
    SupportTwo[x] = y + 2u < SimulationSize.y
                        ? StateOut[uint2(x, y + 2u)]
                        : IRON;
  }
  GroupMemoryBarrierWithGroupSync();

  bool scanRight = ((y + FrameIndex) & 1u) == 0u;
  TransferSharedLiquidPressure(lane, WATER, scanRight);
  TransferSharedLiquidPressure(lane, LAVA, !scanRight);

  for (uint x = lane; x < SimulationSize.x; x += ROW_THREADS)
    StateOut[uint2(x, y)] = RowState[x];
}

VertexOutput FullscreenVertex(uint id : SV_VertexID)
{
  VertexOutput output;
  output.uv = float2((id << 1) & 2, id & 2);
  output.position =
      float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return output;
}

float4 PixelColor(uint pixel)
{
  pixel = PixelType(pixel);
  if (pixel == WOOD)
    return float4(0.38, 0.20, 0.07, 1.0);
  if (pixel == IRON)
    return float4(0.38, 0.42, 0.45, 1.0);
  if (pixel == SAND)
    return float4(0.82, 0.67, 0.30, 1.0);
  if (pixel == RUST)
    return float4(0.56, 0.18, 0.05, 1.0);
  if (pixel == WATER)
    return float4(0.05, 0.34, 0.82, 1.0);
  if (pixel == LAVA)
    return float4(1.0, 0.20, 0.01, 1.0);
  if (pixel == SMOKE)
    return float4(0.22, 0.23, 0.25, 1.0);
  if (pixel == STEAM)
    return float4(0.78, 0.84, 0.88, 1.0);
  return float4(0.025, 0.03, 0.045, 1.0);
}

float4 DrawPixels(VertexOutput input) : SV_Target
{
  uint2 viewportPixel = uint2(input.position.xy) - ViewportOrigin;
  uint2 cell =
      min(viewportPixel * SimulationSize / ViewportSize, SimulationSize - 1u);
  float4 color = PixelColor(StateIn[cell]);

  // D3D11 owns this surface, so draw the clickable material palette in the same
  // pass.
  int2 palettePosition = int2(cell) - int2(8, 6);
  int button = palettePosition.x / 52 + 1;
  int localX = palettePosition.x - (button - 1) * 52;
  bool insidePalette = palettePosition.x >= 0 && palettePosition.y >= 0 &&
                       palettePosition.y < 14 && button >= int(WOOD) &&
                       button <= int(STEAM) && localX < 48;
  if (insidePalette)
  {
    uint material = uint(button);
    float4 swatch = PixelColor(material);
    bool selected = material == SelectedPixel;
    bool border = localX < 2 || localX >= 46 || palettePosition.y < 2 ||
                  palettePosition.y >= 12;
    color = selected && border ? float4(1.0, 1.0, 1.0, 1.0) : swatch;
  }
  return color;
}
