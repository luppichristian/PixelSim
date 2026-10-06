[numthreads(8, 8, 1)] void apply_brush(uint3 id : SV_DispatchThreadID)
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

[numthreads(8, 8, 1)] void simulate(uint3 id : SV_DispatchThreadID)
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

  uint a = read_state(topLeft);
  uint b = read_state(topRight);
  uint c = read_state(bottomLeft);
  uint d = read_state(bottomRight);
  uint random = hash(id.xy, FrameIndex);

  react_block(a, b, c, d, random);

  // Four non-overlapping 2x2 phases give every output cell one writer while
  // allowing particles to cross each block edge on a later update.
  // The column-owned pass handles direct accelerated falling. This local
  // pass only rolls powder diagonally when the cell directly below is blocked.
  if (is_powder(a) && !can_fall_into(a, c) && can_fall_into(a, d))
    swap_pixels(a, d);
  if (is_powder(b) && !can_fall_into(b, d) && can_fall_into(b, c))
    swap_pixels(b, c);

  rise_pair(a, c);
  rise_pair(b, d);

  // Gases diffuse horizontally without creating or deleting cells.
  if ((random & 2u) != 0u)
  {
    if (is_gas(a) && pixel_type(b) == EMPTY)
      swap_pixels(a, b);
    if (is_gas(c) && pixel_type(d) == EMPTY)
      swap_pixels(c, d);
  }
  else
  {
    if (pixel_type(a) == EMPTY && is_gas(b))
      swap_pixels(a, b);
    if (pixel_type(c) == EMPTY && is_gas(d))
      swap_pixels(c, d);
  }

  if (pixel_type(a) == STEAM && (random & 1023u) == 0u)
    a = WATER;
  if (pixel_type(b) == STEAM && ((random >> 2) & 1023u) == 0u)
    b = WATER;
  if (pixel_type(c) == STEAM && ((random >> 4) & 1023u) == 0u)
    c = WATER;
  if (pixel_type(d) == STEAM && ((random >> 6) & 1023u) == 0u)
    d = WATER;

  age_fire(a);
  age_fire(b);
  age_fire(c);
  age_fire(d);

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
void move_falling_materials_vertical(uint3 id : SV_DispatchThreadID)
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
    if (!is_liquid(particle) && !is_powder(particle))
      continue;

    uint speed = min(velocity_y(particle) + 1u, uint(MAX_FALL_SPEED));
    int destinationY = sourceY;
    for (uint distance = 1u; distance <= speed; ++distance)
    {
      int targetY = sourceY + int(distance);
      if (targetY >= int(SimulationSize.y))
        break;
      if (!can_fall_into(particle, StateOut[int2(int(x), targetY)]))
        break;
      destinationY = targetY;
    }

    if (destinationY == sourceY)
    {
      StateOut[source] = with_velocity_y(particle, 0u);
      continue;
    }

    int2 destination = int2(int(x), destinationY);
    uint resident = StateOut[destination];
    StateOut[destination] = with_velocity_y(particle, speed);
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

bool is_shared_pressure_source(uint x, uint liquidType)
{
  uint pixel = RowState[x];
  return pixel_type(pixel) == liquidType && velocity_y(pixel) == 0u &&
         pixel_type(SupportOne[x]) == liquidType &&
         !can_fall_into(SupportOne[x], SupportTwo[x]);
}

bool is_shared_pressure_destination(uint x, uint liquidType)
{
  return is_open(RowState[x]) && pixel_type(SupportOne[x]) != liquidType;
}

void transfer_shared_liquid_pressure(uint lane, uint liquidType, bool scanRight)
{
  for (uint x = lane; x < SimulationSize.x; x += ROW_THREADS)
  {
    uint type = pixel_type(RowState[x]);
    SegmentId[x] = type != liquidType && !is_open(RowState[x]) ? 1u : 0u;
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
    uint type = pixel_type(RowState[x]);
    if (type == liquidType || is_open(RowState[x]))
    {
      uint segment = SegmentId[x];
      if (is_shared_pressure_source(x, liquidType))
      {
        InterlockedMin(SourceForward[segment], x);
        InterlockedMax(SourceBackward[segment], x + 1u);
      }
    }
  }
  GroupMemoryBarrierWithGroupSync();

  for (uint x = lane; x < SimulationSize.x; x += ROW_THREADS)
  {
    uint type = pixel_type(RowState[x]);
    if ((type == liquidType || is_open(RowState[x])) &&
        is_shared_pressure_destination(x, liquidType))
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
    uint type = pixel_type(RowState[x]);
    bool traversable = type == liquidType || is_open(RowState[x]);
    bool segmentStart = false;
    if (traversable)
    {
      segmentStart = x == 0u;
      if (x > 0u)
        segmentStart = pixel_type(RowState[x - 1u]) != liquidType &&
                       !is_open(RowState[x - 1u]);
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
      swap_pixels(RowState[source], RowState[destination]);
  }
  GroupMemoryBarrierWithGroupSync();
}

[numthreads(ROW_THREADS, 1, 1)]
void move_liquids_horizontal(uint3 groupId : SV_GroupID,
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
  transfer_shared_liquid_pressure(lane, WATER, scanRight);
  transfer_shared_liquid_pressure(lane, LAVA, !scanRight);
  transfer_shared_liquid_pressure(lane, ACID, scanRight);
  transfer_shared_liquid_pressure(lane, OIL, !scanRight);

  for (uint x = lane; x < SimulationSize.x; x += ROW_THREADS)
    StateOut[uint2(x, y)] = RowState[x];
}
