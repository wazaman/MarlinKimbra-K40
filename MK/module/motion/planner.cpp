/**
 * planner.cpp - Buffer movement commands and manage the acceleration profile plan
 * Part of Grbl
 *
 * Copyright (c) 2009-2011 Simen Svale Skogsrud
 *
 * Grbl is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Grbl is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * The ring buffer implementation gleaned from the wiring_serial library by David A. Mellis.
 *
 *
 * Reasoning behind the mathematics in this module (in the key of 'Mathematica'):
 *
 * s == speed, a == acceleration, t == time, d == distance
 *
 * Basic definitions:
 *   Speed[s_, a_, t_] := s + (a*t)
 *   Travel[s_, a_, t_] := Integrate[Speed[s, a, t], t]
 *
 * Distance to reach a specific speed with a constant acceleration:
 *   Solve[{Speed[s, a, t] == m, Travel[s, a, t] == d}, d, t]
 *   d -> (m^2 - s^2)/(2 a) --> estimate_acceleration_distance()
 *
 * Speed after a given distance of travel with constant acceleration:
 *   Solve[{Speed[s, a, t] == m, Travel[s, a, t] == d}, m, t]
 *   m -> Sqrt[2 a d + s^2]
 *
 * DestinationSpeed[s_, a_, d_] := Sqrt[2 a d + s^2]
 *
 * When to start braking (di) to reach a specified destination speed (s2) after accelerating
 * from initial speed s1 without ever stopping at a plateau:
 *   Solve[{DestinationSpeed[s1, a, di] == DestinationSpeed[s2, a, d - di]}, di]
 *   di -> (2 a d - s1^2 + s2^2)/(4 a) --> intersection_distance()
 *
 * IntersectionDistance[s1_, s2_, a_, d_] := (2 a d - s1^2 + s2^2)/(4 a)
 *
 */

#include "../../base.h"
#include "planner.h"

//===========================================================================
//============================= public variables ============================
//===========================================================================

millis_t minsegmenttime;
float max_feedrate[3 + EXTRUDERS]; // Max speeds in mm per minute
float axis_steps_per_unit[3 + EXTRUDERS];
unsigned long max_acceleration_units_per_sq_second[3 + EXTRUDERS]; // Use M201 to override by software
float minimumfeedrate;
float acceleration;                     // Normal acceleration mm/s^2  DEFAULT ACCELERATION for all printing moves. M204 SXXXX
float retract_acceleration[EXTRUDERS];  // mm/s^2 filament pull-back and push-forward while standing still in the other axes M204 TXXXX
float travel_acceleration;              // Travel acceleration mm/s^2  DEFAULT ACCELERATION for all NON printing moves. M204 MXXXX
float max_xy_jerk;                      // The largest speed change requiring no acceleration
float max_z_jerk;
float max_e_jerk[EXTRUDERS];            // mm/s - initial speed for extruder retract moves
float mintravelfeedrate;
unsigned long axis_steps_per_sqr_second[3 + EXTRUDERS];
uint8_t last_extruder;

#if ENABLED(AUTO_BED_LEVELING_FEATURE)
  // Transform required to compensate for bed level
  matrix_3x3 plan_bed_level_matrix = {
    1.0, 0.0, 0.0,
    0.0, 1.0, 0.0,
    0.0, 0.0, 1.0
  };
#endif // AUTO_BED_LEVELING_FEATURE

#if ENABLED(AUTOTEMP)
  float autotemp_max = 250;
  float autotemp_min = 210;
  float autotemp_factor = 0.1;
  bool autotemp_enabled = false;
#endif

//===========================================================================
//============ semi-private variables, used in inline functions =============
//===========================================================================

block_t block_buffer[BLOCK_BUFFER_SIZE];            // A ring buffer for motion instfructions
volatile unsigned char block_buffer_head;           // Index of the next block to be pushed
volatile unsigned char block_buffer_tail;           // Index of the block to process now

//===========================================================================
//============================ private variables ============================
//===========================================================================

// The current position of the tool in absolute steps
long position[NUM_AXIS];               // Rescaled from extern when axis_steps_per_unit are changed by gcode
static float previous_speed[NUM_AXIS]; // Speed of previous path line segment
static float previous_nominal_speed;   // Nominal speed of previous path line segment

uint8_t g_uc_extruder_last_move[EXTRUDERS] = { 0 };

#if ENABLED(XY_FREQUENCY_LIMIT)
  // Used for the frequency limit
  #define MAX_FREQ_TIME (1000000.0/XY_FREQUENCY_LIMIT)
  // Old direction bits. Used for speed calculations
  static unsigned char old_direction_bits = 0;
  // Segment times (in µs). Used for speed calculations
  static long axis_segment_time[2][3] = { {MAX_FREQ_TIME + 1, 0, 0}, {MAX_FREQ_TIME + 1, 0, 0} };
#endif

#if ENABLED(FILAMENT_SENSOR)
  static char meas_sample; // temporary variable to hold filament measurement sample
#endif

#if ENABLED(DUAL_X_CARRIAGE)
  extern bool extruder_duplication_enabled;
#endif

//===========================================================================
//================================ functions ================================
//===========================================================================

// Get the next / previous index of the next block in the ring buffer
// NOTE: Using & here (not %) because BLOCK_BUFFER_SIZE is always a power of 2
FORCE_INLINE int8_t next_block_index(int8_t block_index) { return BLOCK_MOD(block_index + 1); }
FORCE_INLINE int8_t prev_block_index(int8_t block_index) { return BLOCK_MOD(block_index - 1); }

// Calculates the distance (not time) it takes to accelerate from initial_rate to target_rate using the
// given acceleration:
FORCE_INLINE float estimate_acceleration_distance(float initial_rate, float target_rate, float acceleration) {
  if (acceleration == 0) return 0; // acceleration was 0, set acceleration distance to 0
  return (target_rate * target_rate - initial_rate * initial_rate) / (acceleration * 2);
}

// This function gives you the point at which you must start braking (at the rate of -acceleration) if
// you started at speed initial_rate and accelerated until this point and want to end at the final_rate after
// a total travel of distance. This can be used to compute the intersection point between acceleration and
// deceleration in the cases where the trapezoid has no plateau (i.e. never reaches maximum speed)

FORCE_INLINE float intersection_distance(float initial_rate, float final_rate, float acceleration, float distance) {
  if (acceleration == 0) return 0; // acceleration was 0, set intersection distance to 0
  return (acceleration * 2 * distance - initial_rate * initial_rate + final_rate * final_rate) / (acceleration * 4);
}

// Calculates trapezoid parameters so that the entry- and exit-speed is compensated by the provided factors.

void calculate_trapezoid_for_block(block_t* block, float entry_factor, float exit_factor) {
  unsigned long initial_rate = ceil(block->nominal_rate * entry_factor); // (step/min)
  unsigned long final_rate = ceil(block->nominal_rate * exit_factor); // (step/min)

  // Limit minimal step rate (Otherwise the timer will overflow.)
  NOLESS(initial_rate, 120);
  NOLESS(final_rate, 120);

  long acceleration = block->acceleration_st;
  int32_t accelerate_steps = ceil(estimate_acceleration_distance(initial_rate, block->nominal_rate, acceleration));
  int32_t decelerate_steps = floor(estimate_acceleration_distance(block->nominal_rate, final_rate, -acceleration));

  // Calculate the size of Plateau of Nominal Rate.
  int32_t plateau_steps = block->step_event_count - accelerate_steps - decelerate_steps;

  // Is the Plateau of Nominal Rate smaller than nothing? That means no cruising, and we will
  // have to use intersection_distance() to calculate when to abort acceleration and start braking
  // in order to reach the final_rate exactly at the end of this block.
  if (plateau_steps < 0) {
    accelerate_steps = ceil(intersection_distance(initial_rate, final_rate, acceleration, block->step_event_count));
    accelerate_steps = max(accelerate_steps, 0); // Check limits due to numerical round-off
    accelerate_steps = min((uint32_t)accelerate_steps, block->step_event_count);//(We can cast here to unsigned, because the above line ensures that we are above zero)
    plateau_steps = 0;
  }

  #if ENABLED(ADVANCE)
    volatile long initial_advance = block->advance * entry_factor * entry_factor;
    volatile long final_advance = block->advance * exit_factor * exit_factor;
  #endif // ADVANCE

  // block->accelerate_until = accelerate_steps;
  // block->decelerate_after = accelerate_steps+plateau_steps;
  CRITICAL_SECTION_START;  // Fill variables used by the stepper in a critical section
  if (!block->busy) { // Don't update variables if block is busy.
    block->accelerate_until = accelerate_steps;
    block->decelerate_after = accelerate_steps + plateau_steps;
    block->initial_rate = initial_rate;
    block->final_rate = final_rate;
    #if ENABLED(ADVANCE)
      block->initial_advance = initial_advance;
      block->final_advance = final_advance;
    #endif
  }
  CRITICAL_SECTION_END;
}

// Calculates the maximum allowable speed at this point when you must be able to reach target_velocity using the
// acceleration within the allotted distance.
FORCE_INLINE float max_allowable_speed(float acceleration, float target_velocity, float distance) {
  return sqrt(target_velocity * target_velocity - 2 * acceleration * distance);
}

// "Junction jerk" in this context is the immediate change in speed at the junction of two blocks.
// This method will calculate the junction jerk as the euclidean distance between the nominal
// velocities of the respective blocks.
// inline float junction_jerk(block_t *before, block_t *after) {
//  return sqrt(
//    pow((before->speed_x-after->speed_x), 2)+pow((before->speed_y-after->speed_y), 2));
//}

// The kernel called by planner_recalculate() when scanning the plan from last to first entry.
void planner_reverse_pass_kernel(block_t* previous, block_t* current, block_t* next) {
  if (!current) return;
  UNUSED(previous);

  if (next) {
    // If entry speed is already at the maximum entry speed, no need to recheck. Block is cruising.
    // If not, block in state of acceleration or deceleration. Reset entry speed to maximum and
    // check for maximum allowable speed reductions to ensure maximum possible planned speed.
    float max_entry_speed = current->max_entry_speed;
    if (current->entry_speed != max_entry_speed) {

      // If nominal length true, max junction speed is guaranteed to be reached. Only compute
      // for max allowable speed if block is decelerating and nominal length is false.
      if (!current->nominal_length_flag && max_entry_speed > next->entry_speed) {
        current->entry_speed = min(max_entry_speed,
                                   max_allowable_speed(-current->acceleration, next->entry_speed, current->millimeters));
      }
      else {
        current->entry_speed = max_entry_speed;
      }
      current->recalculate_flag = true;

    }
  } // Skip last block. Already initialized and set for recalculation.
}

// planner_recalculate() needs to go over the current plan twice. Once in reverse and once forward. This
// implements the reverse pass.
void planner_reverse_pass() {
  uint8_t block_index = block_buffer_head;

  //Make a local copy of block_buffer_tail, because the interrupt can alter it
  CRITICAL_SECTION_START;
    unsigned char tail = block_buffer_tail;
  CRITICAL_SECTION_END

  if (BLOCK_MOD(block_buffer_head - tail + BLOCK_BUFFER_SIZE) > 3) { // moves queued
    block_index = BLOCK_MOD(block_buffer_head - 3);
    block_t* block[3] = { NULL, NULL, NULL };
    while (block_index != tail) {
      block_index = prev_block_index(block_index);
      block[2] = block[1];
      block[1] = block[0];
      block[0] = &block_buffer[block_index];
      planner_reverse_pass_kernel(block[0], block[1], block[2]);
    }
  }
}

// The kernel called by planner_recalculate() when scanning the plan from first to last entry.
void planner_forward_pass_kernel(block_t* previous, block_t* current, block_t* next) {
  if (!previous) return;
  UNUSED(next);

  // If the previous block is an acceleration block, but it is not long enough to complete the
  // full speed change within the block, we need to adjust the entry speed accordingly. Entry
  // speeds have already been reset, maximized, and reverse planned by reverse planner.
  // If nominal length is true, max junction speed is guaranteed to be reached. No need to recheck.
  if (!previous->nominal_length_flag) {
    if (previous->entry_speed < current->entry_speed) {
      double entry_speed = min(current->entry_speed,
                               max_allowable_speed(-previous->acceleration, previous->entry_speed, previous->millimeters));
      // Check for junction speed change
      if (current->entry_speed != entry_speed) {
        current->entry_speed = entry_speed;
        current->recalculate_flag = true;
      }
    }
  }
}

// planner_recalculate() needs to go over the current plan twice. Once in reverse and once forward. This
// implements the forward pass.
void planner_forward_pass() {
  uint8_t block_index = block_buffer_tail;
  block_t* block[3] = { NULL, NULL, NULL };

  while (block_index != block_buffer_head) {
    block[0] = block[1];
    block[1] = block[2];
    block[2] = &block_buffer[block_index];
    planner_forward_pass_kernel(block[0], block[1], block[2]);
    block_index = next_block_index(block_index);
  }
  planner_forward_pass_kernel(block[1], block[2], NULL);
}

// Recalculates the trapezoid speed profiles for all blocks in the plan according to the
// entry_factor for each junction. Must be called by planner_recalculate() after
// updating the blocks.
void planner_recalculate_trapezoids() {
  int8_t block_index = block_buffer_tail;
  block_t* current;
  block_t* next = NULL;

  while (block_index != block_buffer_head) {
    current = next;
    next = &block_buffer[block_index];
    if (current) {
      // Recalculate if current block entry or exit junction speed has changed.
      if (current->recalculate_flag || next->recalculate_flag) {
        // NOTE: Entry and exit factors always > 0 by all previous logic operations.
        float nom = current->nominal_speed;
        calculate_trapezoid_for_block(current, current->entry_speed / nom, next->entry_speed / nom);
        current->recalculate_flag = false; // Reset current only to ensure next trapezoid is computed
      }
    }
    block_index = next_block_index(block_index);
  }
  // Last/newest block in buffer. Exit speed is set with MINIMUM_PLANNER_SPEED. Always recalculated.
  if (next) {
    float nom = next->nominal_speed;
    calculate_trapezoid_for_block(next, next->entry_speed / nom, (MINIMUM_PLANNER_SPEED) / nom);
    next->recalculate_flag = false;
  }
}

// Recalculates the motion plan according to the following algorithm:
//
//   1. Go over every block in reverse order and calculate a junction speed reduction (i.e. block_t.entry_factor)
//      so that:
//     a. The junction jerk is within the set limit
//     b. No speed reduction within one block requires faster deceleration than the one, true constant
//        acceleration.
//   2. Go over every block in chronological order and dial down junction speed reduction values if
//     a. The speed increase within one block would require faster acceleration than the one, true
//        constant acceleration.
//
// When these stages are complete all blocks have an entry_factor that will allow all speed changes to
// be performed using only the one, true constant acceleration, and where no junction jerk is jerkier than
// the set limit. Finally it will:
//
//   3. Recalculate trapezoids for all blocks.

void planner_recalculate() {
  planner_reverse_pass();
  planner_forward_pass();
  planner_recalculate_trapezoids();
}

void plan_init() {
  block_buffer_head = block_buffer_tail = 0;
  memset(position, 0, sizeof(position)); // clear position
  for (int i = 0; i < NUM_AXIS; i++) previous_speed[i] = 0.0;
  previous_nominal_speed = 0.0;
}

#if ENABLED(AUTOTEMP)
  void getHighESpeed() {
    static float oldt = 0;

    if (!autotemp_enabled) return;
    if (degTargetHotend0() + 2 < autotemp_min) return; // probably temperature set to zero.

    float high = 0.0;
    uint8_t block_index = block_buffer_tail;

    while (block_index != block_buffer_head) {
      block_t* block = &block_buffer[block_index];
      if (block->steps[X_AXIS] || block->steps[Y_AXIS] || block->steps[Z_AXIS]) {
        float se = (float)block->steps[E_AXIS] / block->step_event_count * block->nominal_speed; // mm/sec;
        NOLESS(high, se);
      }
      block_index = next_block_index(block_index);
    }

    float t = autotemp_min + high * autotemp_factor;
    t = constrain(t, autotemp_min, autotemp_max);
    if (oldt > t) {
      t *= (1 - (AUTOTEMP_OLDWEIGHT));
      t += (AUTOTEMP_OLDWEIGHT) * oldt;
    }
    oldt = t;
    setTargetHotend0(t);
  }
#endif //AUTOTEMP

void check_axes_activity() {
  unsigned char axis_active[NUM_AXIS] = { 0 },
                tail_fan_speed = fanSpeed;
  #if ENABLED(BARICUDA)
    unsigned char tail_valve_pressure = ValvePressure,
                  tail_e_to_p_pressure = EtoPPressure;
  #endif
  #if ENABLED(LASERBEAM)
    unsigned char tail_laser_ttl_modulation = laser_ttl_modulation;
  #endif
  block_t* block;

  if (blocks_queued()) {
    uint8_t block_index = block_buffer_tail;
    tail_fan_speed = block_buffer[block_index].fan_speed;
    #if ENABLED(BARICUDA)
      block = &block_buffer[block_index];
      tail_valve_pressure = block->valve_pressure;
      tail_e_to_p_pressure = block->e_to_p_pressure;
    #endif
    #if ENABLED(LASERBEAM)
      tail_laser_ttl_modulation = block_buffer[block_index].laser_ttlmodulation;
    #endif

    while (block_index != block_buffer_head) {
      block = &block_buffer[block_index];
      for (int i = 0; i < NUM_AXIS; i++) if (block->steps[i]) axis_active[i]++;
      block_index = next_block_index(block_index);
    }
  }
  if (DISABLE_X && !axis_active[X_AXIS]) disable_x();
  if (DISABLE_Y && !axis_active[Y_AXIS]) disable_y();
  if (DISABLE_Z && !axis_active[Z_AXIS]) disable_z();
  if (DISABLE_E && !axis_active[E_AXIS]) {
    disable_e0();
    disable_e1();
    disable_e2();
    disable_e3();
  }

  #if HAS(FAN)
    #if ENABLED(FAN_KICKSTART_TIME)
      static millis_t fan_kick_end;
      if (tail_fan_speed) {
        millis_t ms = millis();
        if (fan_kick_end == 0) {
          // Just starting up fan - run at full power.
          fan_kick_end = ms + FAN_KICKSTART_TIME;
          tail_fan_speed = 255;
        }
        else if (fan_kick_end > ms)
          // Fan still spinning up.
          tail_fan_speed = 255;
        }
        else {
          fan_kick_end = 0;
        }
    #endif //FAN_KICKSTART_TIME
    #if ENABLED(FAN_MIN_PWM)
      #define CALC_FAN_SPEED (tail_fan_speed ? ( FAN_MIN_PWM + (tail_fan_speed * (255 - (FAN_MIN_PWM))) / 255 ) : 0)
    #else
      #define CALC_FAN_SPEED tail_fan_speed
    #endif // FAN_MIN_PWM
    #if ENABLED(FAN_SOFT_PWM)
      fanSpeedSoftPwm = CALC_FAN_SPEED;
    #else
      analogWrite(FAN_PIN, CALC_FAN_SPEED);
    #endif // FAN_SOFT_PWM
  #endif // HAS(FAN)

  #if ENABLED(AUTOTEMP)
    getHighESpeed();
  #endif

  #if ENABLED(BARICUDA)
    #if HAS(HEATER_1)
      analogWrite(HEATER_1_PIN, tail_valve_pressure);
    #endif
    #if HAS(HEATER_2)
      analogWrite(HEATER_2_PIN, tail_e_to_p_pressure);
    #endif
  #endif

  // add Laser TTL Modulation(PWM) Control
  #if ENABLED(LASERBEAM)
    analogWrite(LASER_TTL_PIN, tail_laser_ttl_modulation);
  #endif
}

float junction_deviation = 0.1;
// Add a new linear movement to the buffer. steps[X_AXIS], _y and _z is the absolute position in
// mm. Microseconds specify how many microseconds the move should take to perform. To aid acceleration
// calculation the caller must also provide the physical length of the line in millimeters.
#if ENABLED(AUTO_BED_LEVELING_FEATURE)
  void plan_buffer_line(float x, float y, float z, const float& e, float feed_rate, const uint8_t extruder, const uint8_t driver)
#else
  void plan_buffer_line(const float& x, const float& y, const float& z, const float& e, float feed_rate, const uint8_t extruder, const uint8_t driver)
#endif  // AUTO_BED_LEVELING_FEATURE
{

  #if ENABLED(ZWOBBLE)
    // Calculate ZWobble
    zwobble.InsertCorrection(z);
  #endif
  #if ENABLED(HYSTERESIS)
    // Calculate Hysteresis
    hysteresis.InsertCorrection(x, y, z, e);
  #endif

  // Calculate the buffer head after we push this byte
  int next_buffer_head = next_block_index(block_buffer_head);

  // If the buffer is full: good! That means we are well ahead of the robot.
  // Rest here until there is room in the buffer.
  while (block_buffer_tail == next_buffer_head) idle();

  #if ENABLED(AUTO_BED_LEVELING_FEATURE)
    apply_rotation_xyz(plan_bed_level_matrix, x, y, z);
  #endif

  // The target position of the tool in absolute steps
  // Calculate target position in absolute steps
  //this should be done after the wait, because otherwise a M92 code within the gcode disrupts this calculation somehow
  int32_t target[NUM_AXIS];
  target[X_AXIS] = lround(x * axis_steps_per_unit[X_AXIS]);
  target[Y_AXIS] = lround(y * axis_steps_per_unit[Y_AXIS]);
  target[Z_AXIS] = lround(z * axis_steps_per_unit[Z_AXIS]);
  target[E_AXIS] = lround(e * axis_steps_per_unit[E_AXIS + extruder]);

  // If changing extruder have to recalculate current position based on 
  // the steps-per-mm value for the new extruder.
  #if EXTRUDERS > 1
    if(last_extruder != extruder && axis_steps_per_unit[E_AXIS + extruder] != 
                                    axis_steps_per_unit[E_AXIS + last_extruder]) {
      float factor = float(axis_steps_per_unit[E_AXIS + extruder]) /
                     float(axis_steps_per_unit[E_AXIS + last_extruder]);
      position[E_AXIS] = lround(position[E_AXIS] * factor);
    }
  #endif

  int32_t dx = target[X_AXIS] - position[X_AXIS],
          dy = target[Y_AXIS] - position[Y_AXIS],
          dz = target[Z_AXIS] - position[Z_AXIS],
          de = target[E_AXIS] - position[E_AXIS];
  #if MECH(COREXY)
    int32_t da = dx + COREX_YZ_FACTOR * dy;
    int32_t db = dx - COREX_YZ_FACTOR * dy;
  #elif MECH(COREYX)
    int32_t da = dy + COREX_YZ_FACTOR * dx;
    int32_t db = dy - COREX_YZ_FACTOR * dx;
  #elif MECH(COREXZ)
    int32_t da = dx + COREX_YZ_FACTOR * dz;
    int32_t dc = dx - COREX_YZ_FACTOR * dz;
  #elif MECH(COREZX)
    int32_t da = dz + COREX_YZ_FACTOR * dx;
    int32_t dc = dz - COREX_YZ_FACTOR * dx;
  #endif

  #if ENABLED(PREVENT_DANGEROUS_EXTRUDE)
    if (de) {
      #if ENABLED(NPR2)
        if (extruder != 1)
      #endif
        {
          if (degHotend(extruder) < extrude_min_temp && !(debugLevel & DEBUG_DRYRUN)) {
            position[E_AXIS] = target[E_AXIS]; //behave as if the move really took place, but ignore E part
            de = 0; // no difference
            ECHO_LM(ER, SERIAL_ERR_COLD_EXTRUDE_STOP);
          }
        }

      #if ENABLED(PREVENT_LENGTHY_EXTRUDE)
        if (labs(de) > axis_steps_per_unit[E_AXIS + extruder] * (EXTRUDE_MAXLENGTH)) {
          #if ENABLED(EASY_LOAD)
            if (!allow_lengthy_extrude_once) {
          #endif
          position[E_AXIS] = target[E_AXIS]; // Behave as if the move really took place, but ignore E part
          de = 0; // no difference
          ECHO_LM(ER, SERIAL_ERR_LONG_EXTRUDE_STOP);
          #if ENABLED(EASY_LOAD)
            }
            allow_lengthy_extrude_once = false;
          #endif
        }
      #endif // PREVENT_LENGTHY_EXTRUDE
    }
  #endif // PREVENT_DANGEROUS_EXTRUDE

  // Prepare to set up new block
  block_t* block = &block_buffer[block_buffer_head];

  // Mark block as not busy (Not executed by the stepper interrupt)
  block->busy = false;

  // Number of steps for each axis
  #if MECH(COREXY) || MECH(COREYX)
    // corexy planning
    block->steps[A_AXIS] = labs(da);
    block->steps[B_AXIS] = labs(db);
    block->steps[Z_AXIS] = labs(dz);
  #elif MECH(COREXZ) || MECH(COREZX)
    // corexz planning
    block->steps[A_AXIS] = labs(da);
    block->steps[Y_AXIS] = labs(dy);
    block->steps[C_AXIS] = labs(dc);
  #else
    // default non-h-bot planning
    block->steps[X_AXIS] = labs(dx);
    block->steps[Y_AXIS] = labs(dy);
    block->steps[Z_AXIS] = labs(dz);
  #endif

  block->steps[E_AXIS] = labs(de);
  block->steps[E_AXIS] *= volumetric_multiplier[extruder];
  block->steps[E_AXIS] *= extruder_multiplier[extruder];
  block->steps[E_AXIS] /= 100;
  block->step_event_count = max(block->steps[X_AXIS], max(block->steps[Y_AXIS], max(block->steps[Z_AXIS], block->steps[E_AXIS])));

  #if DISABLED(LASER)
  // Bail if this is a zero-length block
  if (block->step_event_count <= DROP_SEGMENTS) return;
  #endif

  block->fan_speed = fanSpeed;

  #if ENABLED(BARICUDA)
    block->valve_pressure = ValvePressure;
    block->e_to_p_pressure = EtoPPressure;
  #endif

  // For a mixing extruder, get steps for each
  #if ENABLED(COLOR_MIXING_EXTRUDER)
    for (uint8_t i = 0; i < DRIVER_EXTRUDERS; i++)
      block->mix_event_count[i] = block->steps[E_AXIS] * mixing_factor[i];
  #endif

  // Add update block variables for LASER BEAM control 
  #if ENABLED(LASERBEAM)
    block->laser_ttlmodulation = laser_ttl_modulation;
  #endif

  // Compute direction bits for this block 
  uint8_t dirb = 0;
  #if MECH(COREXY) || MECH(COREYX)
    if (dx < 0) SBI(dirb, X_HEAD); // Save the real Extruder (head) direction in X Axis
    if (dy < 0) SBI(dirb, Y_HEAD); // ...and Y
    if (dz < 0) SBI(dirb, Z_AXIS);
    if (da < 0) SBI(dirb, A_AXIS); // Motor A direction
    if (db < 0) SBI(dirb, B_AXIS); // Motor B direction
  #elif MECH(COREXZ) || MECH(COREZX)
    if (dx < 0) SBI(dirb, X_HEAD); // Save the real Extruder (head) direction in X Axis
    if (dy < 0) SBI(dirb, Y_AXIS);
    if (dz < 0) SBI(dirb, Z_HEAD); // ...and Z
    if (da < 0) SBI(dirb, A_AXIS); // Motor A direction
    if (dc < 0) SBI(dirb, C_AXIS); // Motor B direction
  #else
    if (dx < 0) SBI(dirb, X_AXIS);
    if (dy < 0) SBI(dirb, Y_AXIS); 
    if (dz < 0) SBI(dirb, Z_AXIS);
  #endif
  if (de < 0) SBI(dirb, E_AXIS); 
  block->direction_bits = dirb;

  block->active_driver = driver;

  // Enable active axes
  #if MECH(COREXY) || MECH(COREYX)
    if (block->steps[A_AXIS] || block->steps[B_AXIS]) {
      enable_x();
      enable_y();
    }
    #if DISABLED(Z_LATE_ENABLE)
      if (block->steps[Z_AXIS]) enable_z();
    #endif
  #elif MECH(COREXZ) || MECH(COREZX)
    if (block->steps[A_AXIS] || block->steps[C_AXIS]) {
      enable_x();
      enable_z();
    }
    if (block->steps[Y_AXIS]) enable_y();
  #else
    if (block->steps[X_AXIS]) enable_x();
    if (block->steps[Y_AXIS]) enable_y();
    #if DISABLED(Z_LATE_ENABLE)
      if (block->steps[Z_AXIS]) enable_z();
    #endif
  #endif

  // Enable extruder(s)
  if (block->steps[E_AXIS]) {
    #if DISABLED(MKR4) && DISABLED(NPR2)
      if (DISABLE_INACTIVE_EXTRUDER) { //enable only selected extruder

        for (int i = 0; i < EXTRUDERS; i++)
          if (g_uc_extruder_last_move[i] > 0) g_uc_extruder_last_move[i]--;

        switch(extruder) {
          case 0:
            enable_e0();
            g_uc_extruder_last_move[0] = (BLOCK_BUFFER_SIZE) * 2;
            #if EXTRUDERS > 1
              if (g_uc_extruder_last_move[1] == 0) disable_e1();
              #if EXTRUDERS > 2
                if (g_uc_extruder_last_move[2] == 0) disable_e2();
                #if EXTRUDERS > 3
                  if (g_uc_extruder_last_move[3] == 0) disable_e3();
                  #if EXTRUDERS > 4
                    if (g_uc_extruder_last_move[4] == 0) disable_e4();
                    #if EXTRUDERS > 5
                      if (g_uc_extruder_last_move[5] == 0) disable_e5();
                    #endif
                  #endif
                #endif
              #endif
            #endif
          break;
          #if EXTRUDERS > 1
            case 1:
              enable_e1();
              g_uc_extruder_last_move[1] = (BLOCK_BUFFER_SIZE) * 2;
              if (g_uc_extruder_last_move[0] == 0) disable_e0();
              #if EXTRUDERS > 2
                if (g_uc_extruder_last_move[2] == 0) disable_e2();
                #if EXTRUDERS > 3
                  if (g_uc_extruder_last_move[3] == 0) disable_e3();
                  #if EXTRUDERS > 4
                    if (g_uc_extruder_last_move[4] == 0) disable_e4();
                    #if EXTRUDERS > 5
                      if (g_uc_extruder_last_move[5] == 0) disable_e5();
                    #endif
                  #endif
                #endif
              #endif
            break;
            #if EXTRUDERS > 2
              case 2:
                enable_e2();
                g_uc_extruder_last_move[2] = (BLOCK_BUFFER_SIZE) * 2;
                if (g_uc_extruder_last_move[0] == 0) disable_e0();
                if (g_uc_extruder_last_move[1] == 0) disable_e1();
                #if EXTRUDERS > 3
                  if (g_uc_extruder_last_move[3] == 0) disable_e3();
                  #if EXTRUDERS > 4
                    if (g_uc_extruder_last_move[4] == 0) disable_e4();
                    #if EXTRUDERS > 5
                      if (g_uc_extruder_last_move[5] == 0) disable_e5();
                    #endif
                  #endif
                #endif
              break;
              #if EXTRUDERS > 3
                case 3:
                  enable_e3();
                  g_uc_extruder_last_move[3] = (BLOCK_BUFFER_SIZE) * 2;
                  if (g_uc_extruder_last_move[0] == 0) disable_e0();
                  if (g_uc_extruder_last_move[1] == 0) disable_e1();
                  if (g_uc_extruder_last_move[2] == 0) disable_e2();
                  #if EXTRUDERS > 4
                    if (g_uc_extruder_last_move[4] == 0) disable_e4();
                    #if EXTRUDERS > 5
                      if (g_uc_extruder_last_move[5] == 0) disable_e5();
                    #endif
                  #endif
                break;
                #if EXTRUDERS > 4
                  case 4:
                    enable_e4();
                    g_uc_extruder_last_move[4] = (BLOCK_BUFFER_SIZE) * 2;
                    if (g_uc_extruder_last_move[0] == 0) disable_e0();
                    if (g_uc_extruder_last_move[1] == 0) disable_e1();
                    if (g_uc_extruder_last_move[2] == 0) disable_e2();
                    if (g_uc_extruder_last_move[3] == 0) disable_e3();
                    #if EXTRUDERS > 5
                      if (g_uc_extruder_last_move[5] == 0) disable_e5();
                    #endif
                  break;
                  #if EXTRUDERS > 5
                    case 4:
                      enable_e5();
                      g_uc_extruder_last_move[5] = (BLOCK_BUFFER_SIZE) * 2;
                      if (g_uc_extruder_last_move[0] == 0) disable_e0();
                      if (g_uc_extruder_last_move[1] == 0) disable_e1();
                      if (g_uc_extruder_last_move[2] == 0) disable_e2();
                      if (g_uc_extruder_last_move[3] == 0) disable_e3();
                      if (g_uc_extruder_last_move[4] == 0) disable_e4();
                    break;
                  #endif // EXTRUDERS > 5
                #endif // EXTRUDERS > 4
              #endif // EXTRUDERS > 3
            #endif // EXTRUDERS > 2
          #endif // EXTRUDERS > 1
        }
      }
      else //enable all
      {
        enable_e0();
        enable_e1();
        enable_e2();
        enable_e3();
        enable_e4();
        enable_e5();
      }
    #else //MKR4 or NPr2
      switch(extruder)
      {
        case 0:
          enable_e0();
        break;
        case 1:
          enable_e1();
        break;
        case 2:
          enable_e0();
        break;
        case 3:
          enable_e1();
        break;
      }
    #endif //!MKR4 && !NPR2
  }

  if (block->steps[E_AXIS])
    NOLESS(feed_rate, minimumfeedrate);
  else
    NOLESS(feed_rate, mintravelfeedrate);

  /**
   * This part of the code calculates the total length of the movement.
   * For cartesian bots, the X_AXIS is the real X movement and same for Y_AXIS.
   * But for corexy bots, that is not true. The "X_AXIS" and "Y_AXIS" motors (that should be named to A_AXIS
   * and B_AXIS) cannot be used for X and Y length, because A=X+Y and B=X-Y.
   * So we need to create other 2 "AXIS", named X_HEAD and Y_HEAD, meaning the real displacement of the Head.
   * Having the real displacement of the head, we can calculate the total movement length and apply the desired speed.
   */
  #if MECH(COREXY) || MECH(COREYX)
    float delta_mm[6];
    delta_mm[X_HEAD] = dx / axis_steps_per_unit[A_AXIS];
    delta_mm[Y_HEAD] = dy / axis_steps_per_unit[B_AXIS];
    delta_mm[Z_AXIS] = dz / axis_steps_per_unit[Z_AXIS];
    delta_mm[A_AXIS] = da / axis_steps_per_unit[A_AXIS];
    delta_mm[B_AXIS] = db / axis_steps_per_unit[B_AXIS];
  #elif MECH(COREXZ) || MECH(COREZX)
    float delta_mm[6];
    delta_mm[X_HEAD] = dx / axis_steps_per_unit[A_AXIS];
    delta_mm[Y_AXIS] = dy / axis_steps_per_unit[Y_AXIS];
    delta_mm[Z_HEAD] = dz / axis_steps_per_unit[C_AXIS];
    delta_mm[A_AXIS] = da / axis_steps_per_unit[A_AXIS];
    delta_mm[C_AXIS] = dc / axis_steps_per_unit[C_AXIS];
  #else
    float delta_mm[4];
    delta_mm[X_AXIS] = dx / axis_steps_per_unit[X_AXIS];
    delta_mm[Y_AXIS] = dy / axis_steps_per_unit[Y_AXIS];
    delta_mm[Z_AXIS] = dz / axis_steps_per_unit[Z_AXIS];
  #endif
  delta_mm[E_AXIS] = (de / axis_steps_per_unit[E_AXIS + extruder]) * volumetric_multiplier[extruder] * extruder_multiplier[extruder] / 100.0;

  if (block->steps[X_AXIS] <= DROP_SEGMENTS && block->steps[Y_AXIS] <= DROP_SEGMENTS && block->steps[Z_AXIS] <= DROP_SEGMENTS) {
    block->millimeters = fabs(delta_mm[E_AXIS]);
  }
  else {
    block->millimeters = sqrt(
      #if MECH(COREXY) || MECH(COREYX)
        square(delta_mm[X_HEAD]) + square(delta_mm[Y_HEAD]) + square(delta_mm[Z_AXIS])
      #elif MECH(COREXZ) || MECH(COREZX)
        square(delta_mm[X_HEAD]) + square(delta_mm[Y_AXIS]) + square(delta_mm[Z_HEAD])
      #else
        square(delta_mm[X_AXIS]) + square(delta_mm[Y_AXIS]) + square(delta_mm[Z_AXIS])
      #endif
    );
  }

  #if ENABLED(LASER)
   block->laser_intensity = laser.intensity;
   block->laser_duration = laser.duration;
   block->laser_status = laser.status;
   block->laser_mode = laser.mode;
    // When operating in PULSED or RASTER modes, laser pulsing must operate in sync with movement.
    // Calculate steps between laser firings (steps_l) and consider that when determining largest
    // interval between steps for X, Y, Z, E, L to feed to the motion control code.
    if (laser.mode == RASTER || laser.mode == PULSED) {
        block->steps_l = labs(block->millimeters*laser.ppm);
       for (int i = 0; i < LASER_MAX_RASTER_LINE; i++) {

            //Scale the image intensity based on the raster power.
         //100% power on a pixel basis is 255, convert back to 255 = 100.

        //http://stackoverflow.com/questions/929103/convert-a-number-range-to-another-range-maintaining-ratio
			int OldRange, NewRange, NewMin;
			//float NewValue;
			NewMin = 580; // Min laser power for raster engraving, still needs to be included into the M649 command
			//OldRange = (255 - 0);
			//NewRange = (laser.rasterlaserpower - NewMin); //7% power on my unit outputs hardly any noticable burn at F3000 on paper, so adjust the raster contrast based off 7 being the lower. 7 still produces burns at slower feed rates, but getting less power than this isn't typically needed at slow feed rates.
			//NewValue = (float)(((((float)laser.raster_data[i] - 0) * NewRange) / OldRange) + 70);
			//NewValue = ((((laser.raster_data[i] - 0) * NewRange) / OldRange) + NewMin);
			//If less than 7%, turn off the laser tube.
			//if(NewValue == 280) 
			//	NewValue = 0;
			int NewValue = laser.raster_data[i];
			NewValue = map(NewValue, 0 ,256, NewMin, laser.rasterlaserpower); // Chnged by Downunder35m as the original mapping resulted in a loss of CPU power due to the float calculations and the input range was set to 260 instead of 255 as otherwise total black areas would not come out properly.
			
			
			
			block->laser_raster_data[i] = NewValue; 
        }
    } else {
        block->steps_l = 0;
    }
    // NEXTIME
    block->step_event_count = max(block->steps[X_AXIS], max(block->steps[Y_AXIS], max(block->steps[Z_AXIS], max(block->steps[E_AXIS], block->steps_l))));

    if (laser.diagnostics) {
      if (block->laser_status == LASER_ON) {
         ECHO_LM(INFO, "Laser firing enabled");
       }
    }
  #endif // LASER


  float inverse_millimeters = 1.0 / block->millimeters;  // Inverse millimeters to remove multiple divides

  // Calculate speed in mm/second for each axis. No divide by zero due to previous checks.
  float inverse_second = feed_rate * inverse_millimeters;

  int moves_queued = movesplanned();

  // Slow down when the buffer starts to empty, rather than wait at the corner for a buffer refill
  #if ENABLED(OLD_SLOWDOWN) || ENABLED(SLOWDOWN)
    bool mq = moves_queued > 1 && moves_queued < (BLOCK_BUFFER_SIZE) / 2;
    #if ENABLED(OLD_SLOWDOWN)
      if (mq) feed_rate *= 2.0 * moves_queued / (BLOCK_BUFFER_SIZE);
    #endif
    #if ENABLED(SLOWDOWN)
      //  segment time im micro seconds
      unsigned long segment_time = lround(1000000.0 / inverse_second);
      if (mq) {
        if (segment_time < minsegmenttime) {
          // buffer is draining, add extra time.  The amount of time added increases if the buffer is still emptied more.
          inverse_second = 1000000.0 / (segment_time + lround(2 * (minsegmenttime - segment_time) / moves_queued));
          #if ENABLED(XY_FREQUENCY_LIMIT)
            segment_time = lround(1000000.0 / inverse_second);
          #endif
        }
      }
    #endif
  #endif

  block->nominal_speed = block->millimeters * inverse_second; // (mm/sec) Always > 0
  block->nominal_rate = ceil(block->step_event_count * inverse_second); // (step/sec) Always > 0

  #if ENABLED(FILAMENT_SENSOR)
    //FMM update ring buffer used for delay with filament measurements

    if (extruder == FILAMENT_SENSOR_EXTRUDER_NUM && delay_index2 > -1) {  //only for extruder with filament sensor and if ring buffer is initialized

      const int MMD = MAX_MEASUREMENT_DELAY + 1, MMD10 = MMD * 10;

      delay_dist += delta_mm[E_AXIS];  // increment counter with next move in e axis
      while (delay_dist >= MMD10) delay_dist -= MMD10; // loop around the buffer
      while (delay_dist < 0) delay_dist += MMD10;

      delay_index1 = delay_dist / 10.0;  // calculate index
      delay_index1 = constrain(delay_index1, 0, MAX_MEASUREMENT_DELAY); // (already constrained above)

      if (delay_index1 != delay_index2) { // moved index
        meas_sample = widthFil_to_size_ratio() - 100;  // Subtract 100 to reduce magnitude - to store in a signed char
        while (delay_index1 != delay_index2) {
          // Increment and loop around buffer
          if (++delay_index2 >= MMD) delay_index2 -= MMD;
          delay_index2 = constrain(delay_index2, 0, MAX_MEASUREMENT_DELAY);
          measurement_delay[delay_index2] = meas_sample;
        }
      }
    }
  #endif

  // Calculate and limit speed in mm/sec for each axis
  float current_speed[NUM_AXIS];
  float speed_factor = 1.0; //factor <=1 do decrease speed
  for (int i = 0; i < NUM_AXIS; i++) {
    current_speed[i] = delta_mm[i] * inverse_second;
    float cs = fabs(current_speed[i]), mf = max_feedrate[i];
    if (cs > mf) speed_factor = min(speed_factor, mf / cs);
  }

  // Max segement time in us.
  #if ENABLED(XY_FREQUENCY_LIMIT)

    // Check and limit the xy direction change frequency
    unsigned char direction_change = block->direction_bits ^ old_direction_bits;
    old_direction_bits = block->direction_bits;
    segment_time = lround((float)segment_time / speed_factor);

    long xs0 = axis_segment_time[X_AXIS][0],
         xs1 = axis_segment_time[X_AXIS][1],
         xs2 = axis_segment_time[X_AXIS][2],
         ys0 = axis_segment_time[Y_AXIS][0],
         ys1 = axis_segment_time[Y_AXIS][1],
         ys2 = axis_segment_time[Y_AXIS][2];

    if (TEST(direction_change, X_AXIS)) {
      xs2 = axis_segment_time[X_AXIS][2] = xs1;
      xs1 = axis_segment_time[X_AXIS][1] = xs0;
      xs0 = 0;
    }
    xs0 = axis_segment_time[X_AXIS][0] = xs0 + segment_time;

    if (TEST(direction_change, Y_AXIS)) {
      ys2 = axis_segment_time[Y_AXIS][2] = axis_segment_time[Y_AXIS][1];
      ys1 = axis_segment_time[Y_AXIS][1] = axis_segment_time[Y_AXIS][0];
      ys0 = 0;
    }
    ys0 = axis_segment_time[Y_AXIS][0] = ys0 + segment_time;

    long max_x_segment_time = max(xs0, max(xs1, xs2)),
         max_y_segment_time = max(ys0, max(ys1, ys2)),
         min_xy_segment_time = min(max_x_segment_time, max_y_segment_time);
    if (min_xy_segment_time < MAX_FREQ_TIME) {
      float low_sf = speed_factor * min_xy_segment_time / (MAX_FREQ_TIME);
      speed_factor = min(speed_factor, low_sf);
    }
  #endif // XY_FREQUENCY_LIMIT

  // Correct the speed
  if (speed_factor < 1.0) {
    for (unsigned char i = 0; i < NUM_AXIS; i++) current_speed[i] *= speed_factor;
    block->nominal_speed *= speed_factor;
    block->nominal_rate *= speed_factor;
  }

  // Compute and limit the acceleration rate for the trapezoid generator.
  float steps_per_mm = block->step_event_count / block->millimeters;
  long bsx = block->steps[X_AXIS], bsy = block->steps[Y_AXIS], bsz = block->steps[Z_AXIS], bse = block->steps[E_AXIS];
  if (bsx == 0 && bsy == 0 && bsz == 0) {
    block->acceleration_st = ceil(retract_acceleration[extruder] * steps_per_mm); // convert to: acceleration steps/sec^2
  }
  else if (bse == 0) {
    block->acceleration_st = ceil(travel_acceleration * steps_per_mm); // convert to: acceleration steps/sec^2
  }
  else {
    block->acceleration_st = ceil(acceleration * steps_per_mm); // convert to: acceleration steps/sec^2
  }
  // Limit acceleration per axis
  unsigned long acc_st = block->acceleration_st,
                xsteps = axis_steps_per_sqr_second[X_AXIS],
                ysteps = axis_steps_per_sqr_second[Y_AXIS],
                zsteps = axis_steps_per_sqr_second[Z_AXIS],
                esteps = axis_steps_per_sqr_second[E_AXIS + extruder];
  if ((float)acc_st * bsx / block->step_event_count > xsteps) acc_st = xsteps;
  if ((float)acc_st * bsy / block->step_event_count > ysteps) acc_st = ysteps;
  if ((float)acc_st * bsz / block->step_event_count > zsteps) acc_st = zsteps;
  if ((float)acc_st * bse / block->step_event_count > esteps) acc_st = esteps;

  block->acceleration_st = acc_st;
  block->acceleration = acc_st / steps_per_mm;

  #ifdef __SAM3X8E__
    block->acceleration_rate = (long)(acc_st * (4294967296.0 / (HAL_TIMER_RATE)));
  #else
    block->acceleration_rate = (long)(acc_st * 16777216.0 / (F_CPU / 8.0));
  #endif

  #if 0  // Use old jerk for now
    // Compute path unit vector
    double unit_vec[3];

    unit_vec[X_AXIS] = delta_mm[X_AXIS] * inverse_millimeters;
    unit_vec[Y_AXIS] = delta_mm[Y_AXIS] * inverse_millimeters;
    unit_vec[Z_AXIS] = delta_mm[Z_AXIS] * inverse_millimeters;

    // Compute maximum allowable entry speed at junction by centripetal acceleration approximation.
    // Let a circle be tangent to both previous and current path line segments, where the junction
    // deviation is defined as the distance from the junction to the closest edge of the circle,
    // colinear with the circle center. The circular segment joining the two paths represents the
    // path of centripetal acceleration. Solve for max velocity based on max acceleration about the
    // radius of the circle, defined indirectly by junction deviation. This may be also viewed as
    // path width or max_jerk in the previous grbl version. This approach does not actually deviate
    // from path, but used as a robust way to compute cornering speeds, as it takes into account the
    // nonlinearities of both the junction angle and junction velocity.
    double vmax_junction = MINIMUM_PLANNER_SPEED; // Set default max junction speed

    // Skip first block or when previous_nominal_speed is used as a flag for homing and offset cycles.
    if ((block_buffer_head != block_buffer_tail) && (previous_nominal_speed > 0.0)) {
      // Compute cosine of angle between previous and current path. (prev_unit_vec is negative)
      // NOTE: Max junction velocity is computed without sin() or acos() by trig half angle identity.
      double cos_theta = - previous_unit_vec[X_AXIS] * unit_vec[X_AXIS]
                         - previous_unit_vec[Y_AXIS] * unit_vec[Y_AXIS]
                         - previous_unit_vec[Z_AXIS] * unit_vec[Z_AXIS] ;
      // Skip and use default max junction speed for 0 degree acute junction.
      if (cos_theta < 0.95) {
        vmax_junction = min(previous_nominal_speed, block->nominal_speed);
        // Skip and avoid divide by zero for straight junctions at 180 degrees. Limit to min() of nominal speeds.
        if (cos_theta > -0.95) {
          // Compute maximum junction velocity based on maximum acceleration and junction deviation
          double sin_theta_d2 = sqrt(0.5 * (1.0 - cos_theta)); // Trig half angle identity. Always positive.
          vmax_junction = min(vmax_junction,
                              sqrt(block->acceleration * junction_deviation * sin_theta_d2 / (1.0 - sin_theta_d2)));
        }
      }
    }
  #endif

  // Start with a safe speed
  float vmax_junction = max_xy_jerk / 2;
  float vmax_junction_factor = 1.0;
  float mz2 = max_z_jerk / 2, me2 = max_e_jerk[extruder] / 2;
  float csz = current_speed[Z_AXIS], cse = current_speed[E_AXIS];
  if (fabs(csz) > mz2) vmax_junction = min(vmax_junction, mz2);
  if (fabs(cse) > me2) vmax_junction = min(vmax_junction, me2);
  vmax_junction = min(vmax_junction, block->nominal_speed);
  float safe_speed = vmax_junction;

  if ((moves_queued > 1) && (previous_nominal_speed > 0.0001)) {
    float dsx = current_speed[X_AXIS] - previous_speed[X_AXIS],
          dsy = current_speed[Y_AXIS] - previous_speed[Y_AXIS],
          dsz = fabs(csz - previous_speed[Z_AXIS]),
          dse = fabs(cse - previous_speed[E_AXIS]),
          jerk = sqrt(dsx * dsx + dsy * dsy);

    //    if ((fabs(previous_speed[X_AXIS]) > 0.0001) || (fabs(previous_speed[Y_AXIS]) > 0.0001)) {
    vmax_junction = block->nominal_speed;
    //    }
    if (jerk > max_xy_jerk) vmax_junction_factor = max_xy_jerk / jerk;
    if (dsz > max_z_jerk) vmax_junction_factor = min(vmax_junction_factor, max_z_jerk / dsz);
    if (dse > max_e_jerk[extruder]) vmax_junction_factor = min(vmax_junction_factor, max_e_jerk[extruder] / dse);

    vmax_junction = min(previous_nominal_speed, vmax_junction * vmax_junction_factor); // Limit speed to max previous speed
  }
  block->max_entry_speed = vmax_junction;

  // Initialize block entry speed. Compute based on deceleration to user-defined MINIMUM_PLANNER_SPEED.
  double v_allowable = max_allowable_speed(-block->acceleration, MINIMUM_PLANNER_SPEED, block->millimeters);
  block->entry_speed = min(vmax_junction, v_allowable);

  // Initialize planner efficiency flags
  // Set flag if block will always reach maximum junction speed regardless of entry/exit speeds.
  // If a block can de/ac-celerate from nominal speed to zero within the length of the block, then
  // the current block and next block junction speeds are guaranteed to always be at their maximum
  // junction speeds in deceleration and acceleration, respectively. This is due to how the current
  // block nominal speed limits both the current and next maximum junction speeds. Hence, in both
  // the reverse and forward planners, the corresponding block junction speed will always be at the
  // the maximum junction speed and may always be ignored for any speed reduction checks.
  block->nominal_length_flag = (block->nominal_speed <= v_allowable);
  block->recalculate_flag = true; // Always calculate trapezoid for new block

  // Update previous path unit_vector and nominal speed
  memcpy(previous_speed, current_speed, sizeof(previous_speed)); // previous_speed[] = current_speed[]
  previous_nominal_speed = block->nominal_speed;

  #if ENABLED(ADVANCE)
    // Calculate advance rate
    if (!bse || (!bsx && !bsy && !bsz)) {
      block->advance_rate = 0;
      block->advance = 0;
    }
    else {
      long acc_dist = estimate_acceleration_distance(0, block->nominal_rate, block->acceleration_st);
      float advance = (STEPS_PER_CUBIC_MM_E * EXTRUDER_ADVANCE_K) * (cse * cse * EXTRUSION_AREA * EXTRUSION_AREA) * 256;
      block->advance = advance;
      block->advance_rate = acc_dist ? advance / (float)acc_dist : 0;
    }
    /*
    ECHO_SMV(OK, "advance :", block->advance/256);
    ECHO_EMV("advance rate :", block->advance_rate/256);
    */
  #endif // ADVANCE

  calculate_trapezoid_for_block(block, block->entry_speed / block->nominal_speed, safe_speed / block->nominal_speed);

  // Move buffer head
  block_buffer_head = next_buffer_head;

  // Update position
  memcpy(position, target, sizeof(target)); // position[] = target[]

  planner_recalculate();

  st_wake_up();

} // plan_buffer_line()

#if ENABLED(AUTO_BED_LEVELING_FEATURE)
  vector_3 plan_get_position() {
    vector_3 position = vector_3(st_get_axis_position_mm(X_AXIS), st_get_axis_position_mm(Y_AXIS), st_get_axis_position_mm(Z_AXIS));

    //position.debug("in plan_get position");
    //plan_bed_level_matrix.debug("in plan_get_position");
    matrix_3x3 inverse = matrix_3x3::transpose(plan_bed_level_matrix);
    //inverse.debug("in plan_get inverse");
    position.apply_rotation(inverse);
    //position.debug("after rotation");

    return position;
  }
#endif // AUTO_BED_LEVELING_FEATURE

#if ENABLED(AUTO_BED_LEVELING_FEATURE)
  void plan_set_position(float x, float y, float z, const float& e)
#else
  void plan_set_position(const float& x, const float& y, const float& z, const float& e)
#endif // AUTO_BED_LEVELING_FEATURE
{
  #if ENABLED(AUTO_BED_LEVELING_FEATURE)
    apply_rotation_xyz(plan_bed_level_matrix, x, y, z);
  #endif

  long  nx = position[X_AXIS] = lround(x * axis_steps_per_unit[X_AXIS]),
        ny = position[Y_AXIS] = lround(y * axis_steps_per_unit[Y_AXIS]),
        nz = position[Z_AXIS] = lround(z * axis_steps_per_unit[Z_AXIS]),
        ne = position[E_AXIS] = lround(e * axis_steps_per_unit[E_AXIS + active_extruder]);
  last_extruder = active_extruder;
  st_set_position(nx, ny, nz, ne);
  previous_nominal_speed = 0.0; // Resets planner junction speeds. Assumes start from rest.

  for (uint8_t i = 0; i < NUM_AXIS; i++) previous_speed[i] = 0.0;
}

void plan_set_e_position(const float& e) {
  position[E_AXIS] = lround(e * axis_steps_per_unit[E_AXIS + active_extruder]);
  last_extruder = active_extruder;
  st_set_e_position(position[E_AXIS]);
}

// Calculate the steps/s^2 acceleration rates, based on the mm/s^s
void reset_acceleration_rates() {
  for (uint8_t i = 0; i < 3 + EXTRUDERS; i++)
    axis_steps_per_sqr_second[i] = max_acceleration_units_per_sq_second[i] * axis_steps_per_unit[i];
}