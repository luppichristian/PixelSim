#define EMPTY 0u
#define WOOD 1u
#define IRON 2u
#define SAND 3u
#define RUST 4u
#define WATER 5u
#define LAVA 6u
#define SMOKE 7u
#define STEAM 8u
#define ACID 9u
#define FIRE 10u
#define OIL 11u
#define SALT 12u
#define BEDROCK 13u
#define TYPE_MASK 0xffu
#define FIRE_AGE_SHIFT 8u
#define VELOCITY_Y_SHIFT 16u
#define DISSOLVED_SALT_MASK 0x01000000u
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

struct TextVertexInput
{
  float2 position : POSITION;
  float2 uv : TEXCOORD0;
  float4 color : COLOR0;
};

struct TextVertexOutput
{
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  float4 color : COLOR0;
};

Texture2D<float> FontAtlas : register(t0);
Texture2D<float4> PostSource : register(t2);
Texture2D<float4> BloomSource : register(t3);
SamplerState LinearSampler : register(s1);

uint hash(uint2 p, uint salt)
{
  uint h = p.x * 0x8da6b343u + p.y * 0xd8163841u + salt * 0xcb1ab31fu;
  h ^= h >> 13;
  h *= 0x85ebca6bu;
  return h ^ (h >> 16);
}

uint pixel_type(uint pixel) { return pixel & TYPE_MASK; }
bool is_solid(uint p) { p = pixel_type(p); return p == WOOD || p == IRON || p == BEDROCK; }
bool is_powder(uint p) { p = pixel_type(p); return p == SAND || p == RUST || p == SALT; }
bool is_liquid(uint p) { p = pixel_type(p); return p == WATER || p == LAVA || p == ACID || p == OIL; }
bool is_gas(uint p) { p = pixel_type(p); return p == SMOKE || p == STEAM || p == FIRE; }
bool is_open(uint p) { return pixel_type(p) == EMPTY || is_gas(p); }

uint fire_age(uint pixel) { return (pixel >> FIRE_AGE_SHIFT) & 0xffu; }

void age_fire(inout uint pixel)
{
  if (pixel_type(pixel) != FIRE)
    return;
  uint age = fire_age(pixel);
  pixel = age >= 45u ? SMOKE : FIRE | ((age + 1u) << FIRE_AGE_SHIFT);
}

uint velocity_y(uint pixel)
{
  return (pixel >> VELOCITY_Y_SHIFT) & 0xffu;
}

uint with_velocity_y(uint pixel, uint velocity)
{
  return (pixel & ~(0xffu << VELOCITY_Y_SHIFT)) |
         ((velocity & 0xffu) << VELOCITY_Y_SHIFT);
}

uint read_state(int2 p)
{
  if (p.x < 0 || p.y < 0 || p.x >= int(SimulationSize.x) ||
      p.y >= int(SimulationSize.y))
    return IRON;
  return StateIn[p];
}

bool can_fall_into(uint mover, uint resident)
{
  mover = pixel_type(mover);
  resident = pixel_type(resident);
  if (resident == EMPTY || is_gas(resident))
    return true;
  if (is_powder(mover) && is_liquid(resident) && resident != LAVA)
    return true;
  if (mover == RUST && resident == LAVA)
    return true;
  if (mover == WATER && resident == OIL)
    return true;
  if (mover == ACID && (resident == WATER || resident == OIL))
    return true;
  return false;
}

bool is_stable_support(uint liquid, int2 position)
{
  int2 supportPosition = position + int2(0, 1);
  uint support = read_state(supportPosition);
  if (is_solid(support))
    return true;
  if (is_liquid(support))
  {
    // Two airborne liquid cells still fall as a column. A third liquid cell or
    // stable material below them supplies local pressure, allowing deep piles
    // to spread instead of behaving like rigid powder.
    return pixel_type(support) == pixel_type(liquid) &&
           !can_fall_into(support, read_state(supportPosition + int2(0, 1)));
  }
  if (!is_powder(support))
    return false;

  // A powder is support only when it cannot fall or roll away next.
  return !can_fall_into(support, read_state(supportPosition + int2(0, 1))) &&
         !can_fall_into(support, read_state(supportPosition + int2(-1, 1))) &&
         !can_fall_into(support, read_state(supportPosition + int2(1, 1)));
}

bool is_supported(uint liquid, int2 position)
{
  return is_liquid(liquid) && is_stable_support(liquid, position);
}

void swap_pixels(inout uint first, inout uint second)
{
  uint temporary = first;
  first = second;
  second = temporary;
}

void react_pair(inout uint first, inout uint second, uint random)
{
  uint firstType = pixel_type(first);
  uint secondType = pixel_type(second);
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
  else if ((random & 63u) == 0u &&
           ((firstType == WATER && secondType == SALT) ||
            (firstType == SALT && secondType == WATER)))
  {
    if (firstType == SALT && (second & DISSOLVED_SALT_MASK) == 0u)
    {
      first = EMPTY;
      second |= DISSOLVED_SALT_MASK;
    }
    else if (secondType == SALT && (first & DISSOLVED_SALT_MASK) == 0u)
    {
      second = EMPTY;
      first |= DISSOLVED_SALT_MASK;
    }
  }
  else if ((firstType == FIRE && secondType == WATER) ||
           (firstType == WATER && secondType == FIRE))
  {
    if (firstType == FIRE)
      first = STEAM;
    else
      second = STEAM;
  }
  else if ((firstType == FIRE || firstType == LAVA) &&
           (secondType == WOOD || secondType == OIL))
  {
    second = FIRE;
  }
  else if ((secondType == FIRE || secondType == LAVA) &&
           (firstType == WOOD || firstType == OIL))
  {
    first = FIRE;
  }
  else if (firstType == ACID && secondType != ACID && secondType != WATER &&
           secondType != EMPTY && secondType != FIRE && secondType != SMOKE &&
           secondType != STEAM && secondType != SALT &&
           secondType != BEDROCK)
  {
    second = EMPTY;
  }
  else if (secondType == ACID && firstType != ACID && firstType != WATER &&
           firstType != EMPTY && firstType != FIRE && firstType != SMOKE &&
           firstType != STEAM && firstType != SALT && firstType != BEDROCK)
  {
    first = EMPTY;
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

void react_block(inout uint a, inout uint b, inout uint c, inout uint d,
                uint random)
{
  // Rotate edge priority so dense interfaces have no fixed favored edge.
  uint order = random & 3u;
  if (order == 0u)
  {
    react_pair(a, b, random >> 2);
    react_pair(a, c, random >> 5);
    react_pair(b, d, random >> 8);
    react_pair(c, d, random >> 11);
  }
  else if (order == 1u)
  {
    react_pair(c, d, random >> 11);
    react_pair(b, d, random >> 8);
    react_pair(a, c, random >> 5);
    react_pair(a, b, random >> 2);
  }
  else if (order == 2u)
  {
    react_pair(a, c, random >> 5);
    react_pair(c, d, random >> 11);
    react_pair(a, b, random >> 2);
    react_pair(b, d, random >> 8);
  }
  else
  {
    react_pair(b, d, random >> 8);
    react_pair(a, b, random >> 2);
    react_pair(c, d, random >> 11);
    react_pair(a, c, random >> 5);
  }
}

void rise_pair(inout uint top, inout uint bottom)
{
  if (is_gas(bottom) && pixel_type(top) == EMPTY)
    swap_pixels(top, bottom);
}
