/**
 * stepper.cpp - stepper motor driver: executes motion plans using stepper motors
 * Marlin Firmware
 *
 * Derived from Grbl
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
 */

/* The timer calculations of this module informed by the 'RepRap cartesian firmware' by Zack Smith
   and Philipp Tiefenbacher. */

#include "../../base.h"
#include "stepper.h"
#include "speed_lookuptable.h"

#if HAS(DIGIPOTSS)
  #include <SPI.h>
#endif

//===========================================================================
//============================= public variables ============================
//===========================================================================
block_t* current_block;  // A pointer to the block currently being traced


//===========================================================================
//============================= private variables ===========================
//===========================================================================
//static makes it impossible to be called from outside of this file by extern.!

// Variables used by The Stepper Driver Interrupt
static unsigned char out_bits = 0;        // The next stepping-bits to be output
static unsigned int cleaning_buffer_counter;

#ifdef LASER
static long counter_l;
#endif // LASER

#ifdef LASER_RASTER
static int counter_raster;
#endif // LASER_RASTER



#if ENABLED(Z_DUAL_ENDSTOPS)
  static bool performing_homing = false,
              locked_z_motor = false,
              locked_z2_motor = false;
#endif

// Counter variables for the Bresenham line tracer
static long counter_x, counter_y, counter_z, counter_e;
volatile static unsigned long step_events_completed; // The number of step events executed in the current block

#if ENABLED(ADVANCE)
  static long advance_rate, advance, final_advance = 0;
  static long old_advance = 0;
  static long e_steps[6];
#endif

static long acceleration_time, deceleration_time;
// static unsigned long accelerate_until, decelerate_after, acceleration_rate, initial_rate, final_rate, nominal_rate;
static unsigned short acc_step_rate; // needed for deceleration start point
static uint8_t step_loops;
static uint8_t step_loops_nominal;
static unsigned short OCR1A_nominal;

volatile long endstops_trigsteps[3] = { 0 };
volatile long endstops_stepsTotal, endstops_stepsDone;
static volatile char endstop_hit_bits = 0; // use X_MIN, Y_MIN, Z_MIN and Z_PROBE as BIT value

#if ENABLED(Z_DUAL_ENDSTOPS) || ENABLED(NPR2)
  static uint16_t
#else
  static byte
#endif
    old_endstop_bits = 0; // use X_MIN, X_MAX... Z_MAX, Z_PROBE, Z2_MIN, Z2_MAX, E_MIN

#if ENABLED(ABORT_ON_ENDSTOP_HIT_FEATURE_ENABLED)
  #if ENABLED(ABORT_ON_ENDSTOP_HIT_INIT)
    bool abort_on_endstop_hit = ABORT_ON_ENDSTOP_HIT_INIT;
  #else
    bool abort_on_endstop_hit = false;
  #endif
#endif

#if PIN_EXISTS(MOTOR_CURRENT_PWM_XY)
  int motor_current_setting[3] = DEFAULT_PWM_MOTOR_CURRENT;
#endif

#if ENABLED(COLOR_MIXING_EXTRUDER)
  static long counter_m[DRIVER_EXTRUDERS];
#endif

static bool check_endstops = true;

volatile long count_position[NUM_AXIS] = { 0 }; // Positions of stepper motors, in step units
volatile signed char count_direction[NUM_AXIS] = { 1 };


//===========================================================================
//================================ functions ================================
//===========================================================================

#if ENABLED(DUAL_X_CARRIAGE)
  #define X_APPLY_DIR(v,ALWAYS) \
    if (extruder_duplication_enabled || ALWAYS) { \
      X_DIR_WRITE(v); \
      X2_DIR_WRITE(v); \
    } \
    else { \
      if (current_block->active_driver) X2_DIR_WRITE(v); else X_DIR_WRITE(v); \
    }
  #define X_APPLY_STEP(v,ALWAYS) \
    if (extruder_duplication_enabled || ALWAYS) { \
      X_STEP_WRITE(v); \
      X2_STEP_WRITE(v); \
    } \
    else { \
      if (current_block->active_driver != 0) X2_STEP_WRITE(v); else X_STEP_WRITE(v); \
    }
#else
  #define X_APPLY_DIR(v,Q) X_DIR_WRITE(v)
  #define X_APPLY_STEP(v,Q) X_STEP_WRITE(v)
#endif

#if ENABLED(Y_DUAL_STEPPER_DRIVERS)
  #define Y_APPLY_DIR(v,Q) { Y_DIR_WRITE(v); Y2_DIR_WRITE((v) != INVERT_Y2_VS_Y_DIR); }
  #define Y_APPLY_STEP(v,Q) { Y_STEP_WRITE(v); Y2_STEP_WRITE(v); }
#else
  #define Y_APPLY_DIR(v,Q) Y_DIR_WRITE(v)
  #define Y_APPLY_STEP(v,Q) Y_STEP_WRITE(v)
#endif

#if ENABLED(Z_DUAL_STEPPER_DRIVERS)
  #define Z_APPLY_DIR(v,Q) { Z_DIR_WRITE(v); Z2_DIR_WRITE(v); }
  #if ENABLED(Z_DUAL_ENDSTOPS)
    #define Z_APPLY_STEP(v,Q) \
    if (performing_homing) { \
      if (Z_HOME_DIR > 0) {\
        if (!(TEST(old_endstop_bits, Z_MAX) && (count_direction[Z_AXIS] > 0)) && !locked_z_motor) Z_STEP_WRITE(v); \
        if (!(TEST(old_endstop_bits, Z2_MAX) && (count_direction[Z_AXIS] > 0)) && !locked_z2_motor) Z2_STEP_WRITE(v); \
      } \
      else { \
        if (!(TEST(old_endstop_bits, Z_MIN) && (count_direction[Z_AXIS] < 0)) && !locked_z_motor) Z_STEP_WRITE(v); \
        if (!(TEST(old_endstop_bits, Z2_MIN) && (count_direction[Z_AXIS] < 0)) && !locked_z2_motor) Z2_STEP_WRITE(v); \
      } \
    } \
    else { \
      Z_STEP_WRITE(v); \
      Z2_STEP_WRITE(v); \
    }
  #else
    #define Z_APPLY_STEP(v,Q) { Z_STEP_WRITE(v); Z2_STEP_WRITE(v); }
  #endif
#else
  #define Z_APPLY_DIR(v,Q) Z_DIR_WRITE(v)
  #define Z_APPLY_STEP(v,Q) Z_STEP_WRITE(v)
#endif

#if DISABLED(COLOR_MIXING_EXTRUDER)
  #define E_APPLY_STEP(v,Q) E_STEP_WRITE(v)
#else
  #define E_APPLY_STEP(v,Q)
#endif

// intRes = intIn1 * intIn2 >> 16
// uses:
// r26 to store 0
// r27 to store the byte 1 of the 24 bit result
#define MultiU16X8toH16(intRes, charIn1, intIn2) \
  asm volatile ( \
                 "clr r26 \n\t" \
                 "mul %A1, %B2 \n\t" \
                 "movw %A0, r0 \n\t" \
                 "mul %A1, %A2 \n\t" \
                 "add %A0, r1 \n\t" \
                 "adc %B0, r26 \n\t" \
                 "lsr r0 \n\t" \
                 "adc %A0, r26 \n\t" \
                 "adc %B0, r26 \n\t" \
                 "clr r1 \n\t" \
                 : \
                 "=&r" (intRes) \
                 : \
                 "d" (charIn1), \
                 "d" (intIn2) \
                 : \
                 "r26" \
               )

// intRes = longIn1 * longIn2 >> 24
// uses:
// r26 to store 0
// r27 to store bits 16-23 of the 48bit result. The top bit is used to round the two byte result.
// note that the lower two bytes and the upper byte of the 48bit result are not calculated.
// this can cause the result to be out by one as the lower bytes may cause carries into the upper ones.
// B0 A0 are bits 24-39 and are the returned value
// C1 B1 A1 is longIn1
// D2 C2 B2 A2 is longIn2
//
#define MultiU24X32toH16(intRes, longIn1, longIn2) \
  asm volatile ( \
                 "clr r26 \n\t" \
                 "mul %A1, %B2 \n\t" \
                 "mov r27, r1 \n\t" \
                 "mul %B1, %C2 \n\t" \
                 "movw %A0, r0 \n\t" \
                 "mul %C1, %C2 \n\t" \
                 "add %B0, r0 \n\t" \
                 "mul %C1, %B2 \n\t" \
                 "add %A0, r0 \n\t" \
                 "adc %B0, r1 \n\t" \
                 "mul %A1, %C2 \n\t" \
                 "add r27, r0 \n\t" \
                 "adc %A0, r1 \n\t" \
                 "adc %B0, r26 \n\t" \
                 "mul %B1, %B2 \n\t" \
                 "add r27, r0 \n\t" \
                 "adc %A0, r1 \n\t" \
                 "adc %B0, r26 \n\t" \
                 "mul %C1, %A2 \n\t" \
                 "add r27, r0 \n\t" \
                 "adc %A0, r1 \n\t" \
                 "adc %B0, r26 \n\t" \
                 "mul %B1, %A2 \n\t" \
                 "add r27, r1 \n\t" \
                 "adc %A0, r26 \n\t" \
                 "adc %B0, r26 \n\t" \
                 "lsr r27 \n\t" \
                 "adc %A0, r26 \n\t" \
                 "adc %B0, r26 \n\t" \
                 "mul %D2, %A1 \n\t" \
                 "add %A0, r0 \n\t" \
                 "adc %B0, r1 \n\t" \
                 "mul %D2, %B1 \n\t" \
                 "add %B0, r0 \n\t" \
                 "clr r1 \n\t" \
                 : \
                 "=&r" (intRes) \
                 : \
                 "d" (longIn1), \
                 "d" (longIn2) \
                 : \
                 "r26" , "r27" \
               )

// Some useful constants

#define ENABLE_STEPPER_DRIVER_INTERRUPT()  SBI(TIMSK1, OCIE1A)
#define DISABLE_STEPPER_DRIVER_INTERRUPT() CBI(TIMSK1, OCIE1A)

void endstops_hit_on_purpose() {
  endstop_hit_bits = 0;
}

void checkHitEndstops() {
  if (endstop_hit_bits) {

    #if ENABLED(ABORT_ON_ENDSTOP_HIT_FEATURE_ENABLED)
      if (abort_on_endstop_hit)
        ECHO_SM(ER, SERIAL_ENDSTOPS_HIT);
      else
        ECHO_SM(DB, SERIAL_ENDSTOPS_HIT);
    #else
      ECHO_SM(DB, SERIAL_ENDSTOPS_HIT);
    #endif

    if (TEST(endstop_hit_bits, X_MIN)) {
      ECHO_MV(SERIAL_ENDSTOP_X, (float)endstops_trigsteps[X_AXIS] / axis_steps_per_unit[X_AXIS]);
      LCD_MESSAGEPGM(MSG_ENDSTOPS_HIT MSG_ENDSTOP_XS);
    }
    if (TEST(endstop_hit_bits, Y_MIN)) {
      ECHO_MV(SERIAL_ENDSTOP_Y, (float)endstops_trigsteps[Y_AXIS] / axis_steps_per_unit[Y_AXIS]);
      LCD_MESSAGEPGM(MSG_ENDSTOPS_HIT MSG_ENDSTOP_YS);
    }
    if (TEST(endstop_hit_bits, Z_MIN)) {
      ECHO_MV(SERIAL_ENDSTOP_Z, (float)endstops_trigsteps[Z_AXIS] / axis_steps_per_unit[Z_AXIS]);
      LCD_MESSAGEPGM(MSG_ENDSTOPS_HIT MSG_ENDSTOP_ZS);
    }
    #if ENABLED(Z_PROBE_ENDSTOP)
      if (TEST(endstop_hit_bits, Z_PROBE)) {
        ECHO_MV(SERIAL_ENDSTOP_PROBE, (float)endstops_trigsteps[Z_AXIS] / axis_steps_per_unit[Z_AXIS]);
        LCD_MESSAGEPGM(MSG_ENDSTOPS_HIT MSG_ENDSTOP_ZPS);
      }
    #endif
    #if ENABLED(NPR2)
      if (TEST(endstop_hit_bits, E_MIN)) {
        ECHO_MV(SERIAL_ENDSTOP_E, (float)endstops_trigsteps[E_AXIS] / axis_steps_per_unit[E_AXIS]);
        LCD_MESSAGEPGM(MSG_ENDSTOPS_HIT MSG_ENDSTOP_ES);
      }
    #endif
    ECHO_E;

    #if ENABLED(ABORT_ON_ENDSTOP_HIT_FEATURE_ENABLED)
      if (abort_on_endstop_hit && !(endstop_hit_bits & _BV(Z_PROBE)) && !(endstop_hit_bits & _BV(E_MIN))) {
        #if ENABLED(SDSUPPORT)
          card.sdprinting = false;
          card.closeFile();
        #endif
        for (int i = 0; i < 3; i++) CBI(axis_known_position, i); // not homed anymore
        quickStop(); // kill the planner buffer
        Stop();      // restart by M999
      }
    #endif

    endstops_hit_on_purpose();
  }
}

#if MECH(COREXY) || MECH(COREYX)
  #define CORE_AXIS_2 B_AXIS
#elif MECH(COREXZ) || MECH(COREZX)
  #define CORE_AXIS_2 C_AXIS
#endif

void enable_endstops(bool check) { check_endstops = check; }

// Check endstops - Called from ISR!
inline void update_endstops() {

  #if ENABLED(Z_DUAL_ENDSTOPS) || ENABLED(NPR2)
    uint16_t
  #else
    byte
  #endif
      current_endstop_bits = 0;

  #define _ENDSTOP_PIN(AXIS, MINMAX) AXIS ##_## MINMAX ##_PIN
  #define _ENDSTOP_INVERTING(AXIS, MINMAX) AXIS ##_## MINMAX ##_ENDSTOP_INVERTING
  #define _AXIS(AXIS) AXIS ##_AXIS
  #define _ENDSTOP_HIT(AXIS) SBI(endstop_hit_bits, _ENDSTOP(AXIS, MIN))
  #define _ENDSTOP(AXIS, MINMAX) AXIS ##_## MINMAX

  // SET_ENDSTOP_BIT: set the current endstop bits for an endstop to its status
  #define SET_ENDSTOP_BIT(AXIS, MINMAX) SET_BIT(current_endstop_bits, _ENDSTOP(AXIS, MINMAX), (READ(_ENDSTOP_PIN(AXIS, MINMAX)) != _ENDSTOP_INVERTING(AXIS, MINMAX)))
  // COPY_BIT: copy the value of COPY_BIT to BIT in bits
  #define COPY_BIT(bits, COPY_BIT, BIT) SET_BIT(bits, BIT, TEST(bits, COPY_BIT))
  // TEST_ENDSTOP: test the old and the current status of an endstop
  #define TEST_ENDSTOP(ENDSTOP) (TEST(current_endstop_bits, ENDSTOP) && TEST(old_endstop_bits, ENDSTOP))

  #if MECH(COREXY) || MECH(COREYX)|| MECH(COREXZ) || MECH(COREZX)

    #define _SET_TRIGSTEPS(AXIS) do { \
        float axis_pos = count_position[_AXIS(AXIS)]; \
        if (_AXIS(AXIS) == A_AXIS) \
          axis_pos = (axis_pos + count_position[CORE_AXIS_2]) / 2; \
        else if (_AXIS(AXIS) == CORE_AXIS_2) \
          axis_pos = (count_position[A_AXIS] - axis_pos) / 2; \
        endstops_trigsteps[_AXIS(AXIS)] = axis_pos; \
      } while(0)

  #else

    #define _SET_TRIGSTEPS(AXIS) endstops_trigsteps[_AXIS(AXIS)] = count_position[_AXIS(AXIS)]

  #endif // COREXY || COREYX || COREXZ || COREZX

  #define UPDATE_ENDSTOP(AXIS,MINMAX) do { \
      SET_ENDSTOP_BIT(AXIS, MINMAX); \
      if (TEST_ENDSTOP(_ENDSTOP(AXIS, MINMAX)) && current_block->steps[_AXIS(AXIS)] > 0) { \
        _SET_TRIGSTEPS(AXIS); \
        _ENDSTOP_HIT(AXIS); \
        step_events_completed = current_block->step_event_count; \
      } \
    } while(0)

  #if MECH(COREXY) || MECH(COREYX)|| MECH(COREXZ) || MECH(COREZX)
    // Head direction in -X axis for CoreXY and CoreXZ bots.
    // If Delta1 == -Delta2, the movement is only in Y or Z axis
    if ((current_block->steps[A_AXIS] != current_block->steps[CORE_AXIS_2]) || (TEST(out_bits, A_AXIS) == TEST(out_bits, CORE_AXIS_2))) {
      if (TEST(out_bits, X_HEAD))
  #else
    if (TEST(out_bits, X_AXIS))   // stepping along -X axis (regular Cartesian bot)
  #endif
      { // -direction
        #if ENABLED(DUAL_X_CARRIAGE)
          // with 2 x-carriages, endstops are only checked in the homing direction for the active extruder
          if ((current_block->active_extruder == 0 && X_HOME_DIR == -1) || (current_block->active_extruder != 0 && X2_HOME_DIR == -1))
        #endif
          {
            #if HAS(X_MIN)
              UPDATE_ENDSTOP(X, MIN);
            #endif
          }
      }
      else { // +direction
        #if ENABLED(DUAL_X_CARRIAGE)
          // with 2 x-carriages, endstops are only checked in the homing direction for the active extruder
          if ((current_block->active_extruder == 0 && X_HOME_DIR == 1) || (current_block->active_extruder != 0 && X2_HOME_DIR == 1))
        #endif
          {
            #if HAS(X_MAX)
              UPDATE_ENDSTOP(X, MAX);
            #endif
          }
      }
  #if MECH(COREXY) || MECH(COREYX)|| MECH(COREXZ) || MECH(COREZX)
    }
  #endif

  #if MECH(COREXY) || MECH(COREYX)
    // Head direction in -Y axis for CoreXY bots.
    // If DeltaX == DeltaY, the movement is only in X axis
    if ((current_block->steps[A_AXIS] != current_block->steps[B_AXIS]) || (TEST(out_bits, A_AXIS) != TEST(out_bits, B_AXIS))) {
      if (TEST(out_bits, Y_HEAD))
  #else
      if (TEST(out_bits, Y_AXIS))   // -direction
  #endif
      { // -direction
        #if HAS(Y_MIN)
          UPDATE_ENDSTOP(Y, MIN);
        #endif
      }
      else { // +direction
        #if HAS(Y_MAX)
          UPDATE_ENDSTOP(Y, MAX);
        #endif
      }
  #if MECH(COREXY) || MECH(COREYX)
    }
  #endif

  #if MECH(COREXZ) || MECH(COREZX)
    // Head direction in -Z axis for CoreXZ bots.
    // If DeltaX == DeltaZ, the movement is only in X axis
    if ((current_block->steps[A_AXIS] != current_block->steps[C_AXIS]) || (TEST(out_bits, A_AXIS) != TEST(out_bits, C_AXIS))) {
      if (TEST(out_bits, Z_HEAD))
  #else
      if (TEST(out_bits, Z_AXIS))
  #endif
      { // z -direction
        #if HAS(Z_MIN)

          #if ENABLED(Z_DUAL_ENDSTOPS)
            SET_ENDSTOP_BIT(Z, MIN);
            #if HAS(Z2_MIN)
              SET_ENDSTOP_BIT(Z2, MIN);
            #else
              COPY_BIT(current_endstop_bits, Z_MIN, Z2_MIN);
            #endif

            byte z_test = TEST_ENDSTOP(Z_MIN) | (TEST_ENDSTOP(Z2_MIN) << 1); // bit 0 for Z, bit 1 for Z2

            if (z_test && current_block->steps[Z_AXIS] > 0) { // z_test = Z_MIN || Z2_MIN
              endstops_trigsteps[Z_AXIS] = count_position[Z_AXIS];
              SBI(endstop_hit_bits, Z_MIN);
              if (!performing_homing || (z_test == 0x3))  //if not performing home or if both endstops were trigged during homing...
                step_events_completed = current_block->step_event_count;
            }
          #else // !Z_DUAL_ENDSTOPS

            UPDATE_ENDSTOP(Z, MIN);

          #endif // !Z_DUAL_ENDSTOPS
        #endif // Z_MIN_PIN

        #if ENABLED(Z_PROBE_ENDSTOP)
          UPDATE_ENDSTOP(Z, PROBE);

          if (TEST_ENDSTOP(Z_PROBE)) {
            endstops_trigsteps[Z_AXIS] = count_position[Z_AXIS];
            SBI(endstop_hit_bits, Z_PROBE);
          }
        #endif
      }
      else { // z +direction
        #if HAS(Z_MAX)

          #if ENABLED(Z_DUAL_ENDSTOPS)

            SET_ENDSTOP_BIT(Z, MAX);
            #if HAS(Z2_MAX)
              SET_ENDSTOP_BIT(Z2, MAX);
            #else
              COPY_BIT(current_endstop_bits, Z_MAX, Z2_MAX);
            #endif

            byte z_test = TEST_ENDSTOP(Z_MAX) | (TEST_ENDSTOP(Z2_MAX) << 1); // bit 0 for Z, bit 1 for Z2

            if (z_test && current_block->steps[Z_AXIS] > 0) {  // t_test = Z_MAX || Z2_MAX
              endstops_trigsteps[Z_AXIS] = count_position[Z_AXIS];
              SBI(endstop_hit_bits, Z_MIN);
              if (!performing_homing || (z_test == 0x3))  //if not performing home or if both endstops were trigged during homing...
                step_events_completed = current_block->step_event_count;
            }

          #else // !Z_DUAL_ENDSTOPS

            UPDATE_ENDSTOP(Z, MAX);

          #endif // !Z_DUAL_ENDSTOPS
        #endif // Z_MAX_PIN
      }
  #if MECH(COREXZ) || MECH(COREZX)
    }
  #endif
  #if ENABLED(NPR2)
    UPDATE_ENDSTOP(E, MIN);
  #endif
  old_endstop_bits = current_endstop_bits;
}

//         __________________________
//        /|                        |\     _________________         ^
//       / |                        | \   /|               |\        |
//      /  |                        |  \ / |               | \       s
//     /   |                        |   |  |               |  \      p
//    /    |                        |   |  |               |   \     e
//   +-----+------------------------+---+--+---------------+----+    e
//   |               BLOCK 1            |      BLOCK 2          |    d
//
//                           time ----->
//
//  The trapezoid is the shape the speed curve over time. It starts at block->initial_rate, accelerates
//  first block->accelerate_until step_events_completed, then keeps going at constant speed until
//  step_events_completed reaches block->decelerate_after after which it decelerates until the trapezoid generator is reset.
//  The slope of acceleration is calculated using v = u + at where t is the accumulated timer values of the steps so far.

void st_wake_up() {
  //  TCNT1 = 0;
  ENABLE_STEPPER_DRIVER_INTERRUPT();
}

FORCE_INLINE unsigned short calc_timer(unsigned short step_rate) {
  unsigned short timer;

  NOMORE(step_rate, MAX_STEP_FREQUENCY);

  if(step_rate > (2 * DOUBLE_STEP_FREQUENCY)) { // If steprate > 2*DOUBLE_STEP_FREQUENCY >> step 4 times
    step_rate = (step_rate >> 2) & 0x3fff;
    step_loops = 4;
  }
  else if(step_rate > DOUBLE_STEP_FREQUENCY) { // If steprate > DOUBLE_STEP_FREQUENCY >> step 2 times
    step_rate = (step_rate >> 1) & 0x7fff;
    step_loops = 2;
  }
  else {
    step_loops = 1;
  }

  NOLESS(step_rate, F_CPU / 500000);
  step_rate -= F_CPU / 500000; // Correct for minimal speed
  if (step_rate >= (8 * 256)) { // higher step rate
    unsigned short table_address = (unsigned short)&speed_lookuptable_fast[(unsigned char)(step_rate >> 8)][0];
    unsigned char tmp_step_rate = (step_rate & 0x00ff);
    unsigned short gain = (unsigned short)pgm_read_word_near(table_address + 2);
    MultiU16X8toH16(timer, tmp_step_rate, gain);
    timer = (unsigned short)pgm_read_word_near(table_address) - timer;
  }
  else { // lower step rates
    unsigned short table_address = (unsigned short)&speed_lookuptable_slow[0][0];
    table_address += ((step_rate) >> 1) & 0xfffc;
    timer = (unsigned short)pgm_read_word_near(table_address);
    timer -= (((unsigned short)pgm_read_word_near(table_address + 2) * (unsigned char)(step_rate & 0x0007)) >> 3);
  }

  if (timer < 100) { // (20kHz this should never happen)
    timer = 100;
    ECHO_EMV(SERIAL_STEPPER_TOO_HIGH, step_rate);
  }

  return timer;
}

/**
 * Set the stepper direction of each axis
 *
 *   X_AXIS=A_AXIS and Y_AXIS=B_AXIS for COREXY or COREYX
 *   X_AXIS=A_AXIS and Z_AXIS=C_AXIS for COREXZ or COREZX
 */
void set_stepper_direction(bool onlye) {

  #define SET_STEP_DIR(AXIS) \
    if (TEST(out_bits, AXIS ##_AXIS)) { \
      AXIS ##_APPLY_DIR(INVERT_## AXIS ##_DIR, false); \
      count_direction[AXIS ##_AXIS] = -1; \
    } \
    else { \
      AXIS ##_APPLY_DIR(!INVERT_## AXIS ##_DIR, false); \
      count_direction[AXIS ##_AXIS] = 1; \
    }

  if (!onlye) {
    SET_STEP_DIR(X); // A
    SET_STEP_DIR(Y); // B
    SET_STEP_DIR(Z); // C
  }

  #if DISABLED(ADVANCE)
    if (TEST(out_bits, E_AXIS)) {
      REV_E_DIR();
      count_direction[E_AXIS] = -1;
    }
    else {
      NORM_E_DIR();
      count_direction[E_AXIS] = 1;
    }
  #endif // !ADVANCE
}

// Initializes the trapezoid generator from the current block. Called whenever a new
// block begins.
FORCE_INLINE void trapezoid_generator_reset() {

  if (current_block->direction_bits != out_bits) {
    out_bits = current_block->direction_bits;
    set_stepper_direction();
  }

  #if ENABLED(ADVANCE)
    advance = current_block->initial_advance;
    final_advance = current_block->final_advance;
    // Do E steps + advance steps
    e_steps[current_block->active_driver] += ((advance >> 8) - old_advance);
    old_advance = advance >>8;
  #endif
  deceleration_time = 0;
  // step_rate to timer interval
  OCR1A_nominal = calc_timer(current_block->nominal_rate);
  // make a note of the number of step loops required at nominal speed
  step_loops_nominal = step_loops;
  acc_step_rate = current_block->initial_rate;
  acceleration_time = calc_timer(acc_step_rate);
  OCR1A = acceleration_time;
}

// "The Stepper Driver Interrupt" - This timer interrupt is the workhorse.
// It pops blocks from the block_buffer and executes them by pulsing the stepper pins appropriately.
ISR(TIMER1_COMPA_vect) {

  if (cleaning_buffer_counter) {
    current_block = NULL;
    plan_discard_current_block();
    #if ENABLED(SD_FINISHED_RELEASECOMMAND)
      if ((cleaning_buffer_counter == 1) && (SD_FINISHED_STEPPERRELEASE)) enqueuecommands_P(PSTR(SD_FINISHED_RELEASECOMMAND));
    #endif
    cleaning_buffer_counter--;
    OCR1A = 200;
    return;
  }

  #if ENABLED(LASER)
    if (laser.dur != 0 && (laser.last_firing + laser.dur < micros())) {
      if (laser.diagnostics) ECHO_LM(INFO,"Laser firing duration elapsed, in interrupt handler");
     laser_extinguish();
    }
  #endif

  // If there is no current block, attempt to pop one from the buffer
  if (!current_block) {
    // Anything in the buffer?
    current_block = plan_get_current_block();
    if (current_block) {
      current_block->busy = true;
      trapezoid_generator_reset();

      // Initialize Bresenham counters to 1/2 the ceiling
      long new_count = -(current_block->step_event_count >> 1);
      counter_x = counter_y = counter_z = counter_e = new_count;
      #if ENABLED(LASER)
         counter_l = counter_x;
         laser.dur = current_block->laser_duration;
      #endif

      #if ENABLED(COLOR_MIXING_EXTRUDER)
        for (uint8_t i = 0; i < DRIVER_EXTRUDERS; i++)
          counter_m[i] = new_count;
      #endif

      step_events_completed = 0;

      #if ENABLED(Z_LATE_ENABLE)
        if (current_block->steps[Z_AXIS] > 0) {
          enable_z();
          #if ENABLED(MUVE)
            enable_e();
          #endif
          OCR1A = 2000; // 1ms wait
          return;
        }
      #endif

      #if ENABLED(LASER_RASTER)
         if (current_block->laser_mode == RASTER) counter_raster = 0;
      #endif

      // #if ENABLED(ADVANCE)
      //   e_steps[current_block->active_driver] = 0;
      // #endif
    }
    else {
      OCR1A = 2000; // 1kHz
    }
  }

  if (current_block != NULL) {

    // Update endstops state, if enabled
    if (check_endstops) update_endstops();

    // Continuous firing of the laser during a move happens here, PPM and raster happen further down
    #if ENABLED(LASER)
      if (current_block->laser_mode == CONTINUOUS && current_block->laser_status == LASER_ON) {
         laser_fire(current_block->laser_intensity);
      }
      if (current_block->laser_status == LASER_OFF) {
         if (laser.diagnostics) ECHO_LM(INFO,"Laser status set to off, in interrupt handler");
         laser_extinguish();
      }
    #endif

    // Take multiple steps per interrupt (For high speed moves)
    for (uint8_t i = 0; i < step_loops; i++) {

        MKSERIAL.checkRx(); // Check for serial chars.

      #if ENABLED(ADVANCE)
        counter_e += current_block->steps[E_AXIS];
        if (counter_e > 0) {
          counter_e -= current_block->step_event_count;
          #if DISABLED(COLOR_MIXING_EXTRUDER)
            // Don't step E for mixing extruder
            e_steps[current_block->active_driver] += TEST(out_bits, E_AXIS) ? -1 : 1;
          #endif
        }

        #if ENABLED(COLOR_MIXING_EXTRUDER)
          long dir = TEST(out_bits, E_AXIS) ? -1 : 1;
          for (uint8_t j = 0; j < DRIVER_EXTRUDERS; j++) {
            counter_m[j] += current_block->steps[E_AXIS];
            if (counter_m[j] > 0) {
              counter_m[j] -= current_block->mix_event_count[j];
              e_steps[j] += dir;
            }
          }
        #endif // !COLOR_MIXING_EXTRUDER
      #endif // ADVANCE

      #define _COUNTER(axis) counter_## axis
      #define _APPLY_STEP(AXIS) AXIS ##_APPLY_STEP
      #define _INVERT_STEP_PIN(AXIS) INVERT_## AXIS ##_STEP_PIN

      #define STEP_START(axis, AXIS) \
        _COUNTER(axis) += current_block->steps[_AXIS(AXIS)]; \
        if (_COUNTER(axis) > 0) _APPLY_STEP(AXIS)(!_INVERT_STEP_PIN(AXIS),0);

      #define STEP_START_MIXING \
        for (uint8_t j = 0; j < DRIVER_EXTRUDERS; j++) {  \
          counter_m[j] += current_block->mix_event_count[j];  \
          if (counter_m[j] > 0) En_STEP_WRITE(j, !INVERT_E_STEP_PIN); \
        }

      #define STEP_END(axis, AXIS) \
        if (_COUNTER(axis) > 0) { \
          _COUNTER(axis) -= current_block->step_event_count; \
          count_position[_AXIS(AXIS)] += count_direction[_AXIS(AXIS)]; \
          _APPLY_STEP(AXIS)(_INVERT_STEP_PIN(AXIS),0); \
        }

      #define STEP_END_MIXING \
        for (uint8_t j = 0; j < DRIVER_EXTRUDERS; j++) {  \
          if (counter_m[j] > 0) { \
            counter_m[j] -= current_block->step_event_count;  \
            En_STEP_WRITE(j, INVERT_E_STEP_PIN);  \
          } \
        }

      STEP_START(x, X);
      STEP_START(y, Y);
      STEP_START(z, Z);
      #if DISABLED(ADVANCE)
        STEP_START(e, E);
        #if ENABLED(COLOR_MIXING_EXTRUDER)
          STEP_START_MIXING;
        #endif
      #endif

      #if ENABLED(STEPPER_HIGH_LOW) && STEPPER_HIGH_LOW_DELAY > 0
        HAL::delayMicroseconds(STEPPER_HIGH_LOW_DELAY);
      #endif

      STEP_END(x, X);
      STEP_END(y, Y);
      STEP_END(z, Z);
      #if DISABLED(ADVANCE)
        STEP_END(e, E);
        #if ENABLED(COLOR_MIXING_EXTRUDER)
          STEP_END_MIXING;
        #endif
      #endif

      #if ENABLED(LASER)
        counter_l += current_block->steps_l;
        if (counter_l > 0) {
          if (current_block->laser_mode == PULSED && current_block->laser_status == LASER_ON) { // Pulsed Firing Mode
            laser_fire(current_block->laser_intensity);
            if (laser.diagnostics) {
              ECHO_MV("X: ", counter_x);
              ECHO_MV("Y: ", counter_y);
              ECHO_MV("L: ", counter_l);
            }
          }
          #if ENABLED(LASER_RASTER)
            if (current_block->laser_mode == RASTER && current_block->laser_status == LASER_ON) { // Raster Firing Mode
              // For some reason, when comparing raster power to ppm line burns the rasters were around 2% more powerful
              // going from darkened paper to burning through paper.
              laser_fire(current_block->laser_raster_data[counter_raster]); 
              if (laser.diagnostics) {
                ECHO_MV("Pixel: ", (float)current_block->laser_raster_data[counter_raster]);
              }
              counter_raster++;
            }
          #endif // LASER_RASTER
          counter_l -= current_block->step_event_count;
        }
        if (current_block->laser_duration != 0 && (laser.last_firing + current_block->laser_duration < micros())) {
          if (laser.diagnostics) ECHO_LM(INFO, "Laser firing duration elapsed, in interrupt fast loop");
          laser_extinguish();
        }
      #endif // LASER


      step_events_completed++;
      if (step_events_completed >= current_block->step_event_count) break;
    }
    // Calculate new timer value
    unsigned short timer;
    unsigned short step_rate;
    if (step_events_completed <= (unsigned long)current_block->accelerate_until) {

      MultiU24X32toH16(acc_step_rate, acceleration_time, current_block->acceleration_rate);
      acc_step_rate += current_block->initial_rate;

      // upper limit
      NOMORE(acc_step_rate, current_block->nominal_rate);

      // step_rate to timer interval
      timer = calc_timer(acc_step_rate);
      OCR1A = timer;
      acceleration_time += timer;

      #if ENABLED(ADVANCE)

        advance += advance_rate * step_loops;
        //NOLESS(advance, current_block->advance);

        // Do E steps + advance steps
        #if ENABLED(COLOR_MIXING_EXTRUDER)
          // Move mixing steppers proportionally
          for (uint8_t j = 0; j < DRIVER_EXTRUDERS; j++)
            e_steps[j] += ((advance >> 8) - old_advance) * current_block->step_event_count / current_block->mix_event_count[j];
        #else
          e_steps[current_block->active_driver] += ((advance >> 8) - old_advance);
        #endif

        old_advance = advance >> 8;

      #endif // ADVANCE
    }
    else if (step_events_completed > (unsigned long)current_block->decelerate_after) {
      MultiU24X32toH16(step_rate, deceleration_time, current_block->acceleration_rate);

      if (step_rate <= acc_step_rate) {
        step_rate = acc_step_rate - step_rate; // Decelerate from acceleration end point.
        // lower limit
        NOLESS(step_rate, current_block->final_rate);
      }
      else {
        step_rate = current_block->final_rate;
      }

      // step_rate to timer interval
      timer = calc_timer(step_rate);
      OCR1A = timer;
      deceleration_time += timer;

      #if ENABLED(ADVANCE)
        advance -= advance_rate * step_loops;
        NOLESS(advance, final_advance);

        // Do E steps + advance steps
        uint32_t advance_whole = advance >> 8;

        #if ENABLED(MIXING_EXTRUDER_FEATURE)
          for (uint8_t j = 0; j < DRIVER_EXTRUDERS; j++)
            e_steps[current_block->active_driver] += (advance_whole - old_advance) * current_block->mix_factor[j];
        #else
          e_steps[current_block->active_driver] += advance_whole - old_advance;
        #endif

        old_advance = advance_whole;
      #endif //ADVANCE
    }
    else {
      OCR1A = OCR1A_nominal;
      // ensure we're running at the correct step rate, even if we just came off an acceleration
      step_loops = step_loops_nominal;
    }

    OCR1A = (OCR1A < (TCNT1 + 16)) ? (TCNT1 + 16) : OCR1A;

    // If current block is finished, reset pointer
    if (step_events_completed >= current_block->step_event_count) {
      current_block = NULL;
      plan_discard_current_block();
    }
  }
}

#if ENABLED(ADVANCE)
  unsigned char old_OCR0A;
  // Timer interrupt for E. e_steps is set in the main routine;
  // Timer 0 is shared with millies
  ISR(TIMER0_COMPA_vect) {
    old_OCR0A += 52; // ~10kHz interrupt (250000 / 26 = 9615kHz)
    OCR0A = old_OCR0A;

    #define STEP_E_ONCE(INDEX) \
      if (e_steps[INDEX] != 0) { \
        E## INDEX ##_STEP_WRITE(INVERT_E_STEP_PIN); \
        if (e_steps[INDEX] < 0) { \
          E## INDEX ##_DIR_WRITE(INVERT_E## INDEX ##_DIR); \
          e_steps[INDEX]++; \
        } \
        else if (e_steps[INDEX] > 0) { \
          E## INDEX ##_DIR_WRITE(!INVERT_E## INDEX ##_DIR); \
          e_steps[INDEX]--; \
        } \
        E## INDEX ##_STEP_WRITE(!INVERT_E_STEP_PIN); \
      }

    // Step all E steppers that have steps, up to 4 steps per interrupt
    for (uint8_t i = 0; i < 4; i++) {
      STEP_E_ONCE(0);
      #if DRIVER_EXTRUDERS > 1
        STEP_E_ONCE(1);
        #if DRIVER_EXTRUDERS > 2
          STEP_E_ONCE(2);
          #if DRIVER_EXTRUDERS > 3
            STEP_E_ONCE(3);
            #if DRIVER_EXTRUDERS > 4
              STEP_E_ONCE(4);
              #if DRIVER_EXTRUDERS > 5
                STEP_E_ONCE(5);
              #endif // DRIVER_EXTRUDERS > 5
            #endif // DRIVER_EXTRUDERS > 4
          #endif // DRIVER_EXTRUDERS > 3
        #endif // DRIVER_EXTRUDERS > 2
      #endif // DRIVER_EXTRUDERS > 1
    }
  }
#endif // ADVANCE

void st_init() {
  digipot_init(); //Initialize Digipot Motor Current
  microstep_init(); //Initialize Microstepping Pins

  // initialise TMC Steppers
  #if ENABLED(HAVE_TMCDRIVER)
    tmc_init();
  #endif
    // initialise L6470 Steppers
  #if ENABLED(HAVE_L6470DRIVER)
    L6470_init();
  #endif

  // Initialize Dir Pins
  #if HAS(X_DIR)
    X_DIR_INIT;
  #endif
  #if HAS(X2_DIR)
    X2_DIR_INIT;
  #endif
  #if HAS(Y_DIR)
    Y_DIR_INIT;
    #if ENABLED(Y_DUAL_STEPPER_DRIVERS) && HAS(Y2_DIR)
      Y2_DIR_INIT;
    #endif
  #endif
  #if HAS(Z_DIR)
    Z_DIR_INIT;
    #if ENABLED(Z_DUAL_STEPPER_DRIVERS) && HAS(Z2_DIR)
      Z2_DIR_INIT;
    #endif
  #endif
  #if HAS(E0_DIR)
    E0_DIR_INIT;
  #endif
  #if HAS(E1_DIR)
    E1_DIR_INIT;
  #endif
  #if HAS(E2_DIR)
    E2_DIR_INIT;
  #endif
  #if HAS(E3_DIR)
    E3_DIR_INIT;
  #endif
  #if HAS(E4_DIR)
    E4_DIR_INIT;
  #endif
  #if HAS(E5_DIR)
    E5_DIR_INIT;
  #endif

  //Initialize Enable Pins - steppers default to disabled.

  #if HAS(X_ENABLE)
    X_ENABLE_INIT;
    if (!X_ENABLE_ON) X_ENABLE_WRITE(HIGH);
  #endif
  #if HAS(X2_ENABLE)
    X2_ENABLE_INIT;
    if (!X_ENABLE_ON) X2_ENABLE_WRITE(HIGH);
  #endif
  #if HAS(Y_ENABLE)
    Y_ENABLE_INIT;
    if (!Y_ENABLE_ON) Y_ENABLE_WRITE(HIGH);

  #if ENABLED(Y_DUAL_STEPPER_DRIVERS) && HAS(Y2_ENABLE)
    Y2_ENABLE_INIT;
    if (!Y_ENABLE_ON) Y2_ENABLE_WRITE(HIGH);
  #endif
  #endif
  #if HAS(Z_ENABLE)
    Z_ENABLE_INIT;
    if (!Z_ENABLE_ON) Z_ENABLE_WRITE(HIGH);

    #if ENABLED(Z_DUAL_STEPPER_DRIVERS) && HAS(Z2_ENABLE)
      Z2_ENABLE_INIT;
      if (!Z_ENABLE_ON) Z2_ENABLE_WRITE(HIGH);
    #endif
  #endif
  #if HAS(E0_ENABLE)
    E0_ENABLE_INIT;
    if (!E_ENABLE_ON) E0_ENABLE_WRITE(HIGH);
  #endif
  #if HAS(E1_ENABLE)
    E1_ENABLE_INIT;
    if (!E_ENABLE_ON) E1_ENABLE_WRITE(HIGH);
  #endif
  #if HAS(E2_ENABLE)
    E2_ENABLE_INIT;
    if (!E_ENABLE_ON) E2_ENABLE_WRITE(HIGH);
  #endif
  #if HAS(E3_ENABLE)
    E3_ENABLE_INIT;
    if (!E_ENABLE_ON) E3_ENABLE_WRITE(HIGH);
  #endif
  #if HAS(E4_ENABLE)
    E4_ENABLE_INIT;
    if (!E_ENABLE_ON) E4_ENABLE_WRITE(HIGH);
  #endif
  #if HAS(E5_ENABLE)
    E5_ENABLE_INIT;
    if (!E_ENABLE_ON) E5_ENABLE_WRITE(HIGH);
  #endif

  //Choice E0-E1 or E0-E2 or E1-E3 pin
  #if ENABLED(MKR4) && HAS(E0E1)
    OUT_WRITE_RELE(E0E1_CHOICE_PIN, LOW);
  #endif
  #if ENABLED(MKR4) && HAS(E0E2)
    OUT_WRITE_RELE(E0E2_CHOICE_PIN, LOW);
  #endif
  #if ENABLED(MKR4) && HAS(E0E3)
    OUT_WRITE_RELE(E0E3_CHOICE_PIN, LOW);
  #endif
  #if ENABLED(MKR4) && HAS(E0E4)
    OUT_WRITE_RELE(E0E4_CHOICE_PIN, LOW);
  #endif
  #if ENABLED(MKR4) && HAS(E0E5)
    OUT_WRITE_RELE(E0E5_CHOICE_PIN, LOW);
  #endif
  #if ENABLED(MKR4) && HAS(E1E3)
    OUT_WRITE_RELE(E1E3_CHOICE_PIN, LOW);
  #endif

  //endstops and pullups

  #if HAS(X_MIN)
    SET_INPUT(X_MIN_PIN);
    #if ENABLED(ENDSTOPPULLUP_XMIN)
      PULLUP(X_MIN_PIN, HIGH);
    #endif
  #endif

  #if HAS(Y_MIN)
    SET_INPUT(Y_MIN_PIN);
    #if ENABLED(ENDSTOPPULLUP_YMIN)
      PULLUP(Y_MIN_PIN, HIGH);
    #endif
  #endif

  #if HAS(Z_MIN)
    SET_INPUT(Z_MIN_PIN);
    #if ENABLED(ENDSTOPPULLUP_ZMIN)
      PULLUP(Z_MIN_PIN, HIGH);
    #endif
  #endif

  #if HAS(Z2_MIN)
    SET_INPUT(Z2_MIN_PIN);
    #if ENABLED(ENDSTOPPULLUP_Z2MIN)
      PULLUP(Z2_MIN_PIN, HIGH);
    #endif
  #endif

  #if HAS(E_MIN)
    SET_INPUT(E_MIN_PIN);
    #if ENABLED(ENDSTOPPULLUP_EMIN)
      PULLUP(E_MIN_PIN, HIGH);
    #endif
  #endif

  #if HAS(X_MAX)
    SET_INPUT(X_MAX_PIN);
    #if ENABLED(ENDSTOPPULLUP_XMAX)
      PULLUP(X_MAX_PIN, HIGH);
    #endif
  #endif

  #if HAS(Y_MAX)
    SET_INPUT(Y_MAX_PIN);
    #if ENABLED(ENDSTOPPULLUP_YMAX)
      PULLUP(Y_MAX_PIN, HIGH);
    #endif
  #endif

  #if HAS(Z_MAX)
    SET_INPUT(Z_MAX_PIN);
    #if ENABLED(ENDSTOPPULLUP_ZMAX)
      PULLUP(Z_MAX_PIN, HIGH);
    #endif
  #endif

  #if HAS(Z2_MAX)
    SET_INPUT(Z2_MAX_PIN);
    #if ENABLED(ENDSTOPPULLUP_Z2MAX)
      PULLUP(Z2_MAX_PIN, HIGH);
    #endif
  #endif

  #if HAS(Z_PROBE) // Check for Z_PROBE_ENDSTOP so we don't pull a pin high unless it's to be used.
    SET_INPUT(Z_PROBE_PIN);
    #if ENABLED(ENDSTOPPULLUP_ZPROBE)
      PULLUP(Z_PROBE_PIN, HIGH);
    #endif
  #endif

  #define _STEP_INIT(AXIS) AXIS ##_STEP_INIT
  #define _WRITE_STEP(AXIS, HIGHLOW) AXIS ##_STEP_WRITE(HIGHLOW)
  #define _DISABLE(axis) disable_## axis()

  #define AXIS_INIT(axis, AXIS, PIN) \
    _STEP_INIT(AXIS); \
    _WRITE_STEP(AXIS, _INVERT_STEP_PIN(PIN)); \
    _DISABLE(axis)

  #define E_AXIS_INIT(NUM) AXIS_INIT(e## NUM, E## NUM, E)

  // Initialize Step Pins
  #if HAS(X_STEP)
    AXIS_INIT(x, X, X);
  #endif
  #if HAS(X2_STEP)
    AXIS_INIT(x, X2, X);
  #endif
  #if HAS(Y_STEP)
    #if ENABLED(Y_DUAL_STEPPER_DRIVERS) && HAS(Y2_STEP)
      Y2_STEP_INIT;
      Y2_STEP_WRITE(INVERT_Y_STEP_PIN);
    #endif
    AXIS_INIT(y, Y, Y);
  #endif
  #if HAS(Z_STEP)
    #if ENABLED(Z_DUAL_STEPPER_DRIVERS) && HAS(Z2_STEP)
      Z2_STEP_INIT;
      Z2_STEP_WRITE(INVERT_Z_STEP_PIN);
    #endif
    AXIS_INIT(z, Z, Z);
  #endif
  #if HAS(E0_STEP)
    E_AXIS_INIT(0);
  #endif
  #if HAS(E1_STEP)
    E_AXIS_INIT(1);
  #endif
  #if HAS(E2_STEP)
    E_AXIS_INIT(2);
  #endif
  #if HAS(E3_STEP)
    E_AXIS_INIT(3);
  #endif
  #if HAS(E4_STEP)
    E_AXIS_INIT(4);
  #endif
  #if HAS(E5_STEP)
    E_AXIS_INIT(5);
  #endif

  // waveform generation = 0100 = CTC
  CBI(TCCR1B, WGM13);
  SBI(TCCR1B, WGM12);
  CBI(TCCR1A, WGM11);
  CBI(TCCR1A, WGM10);

  // output mode = 00 (disconnected)
  TCCR1A &= ~(3 << COM1A0);
  TCCR1A &= ~(3 << COM1B0);
  // Set the timer pre-scaler
  // Generally we use a divider of 8, resulting in a 2MHz timer
  // frequency on a 16MHz MCU. If you are going to change this, be
  // sure to regenerate speed_lookuptable.h with
  // create_speed_lookuptable.py
  TCCR1B = (TCCR1B & ~(0x07 << CS10)) | (2 << CS10);

  OCR1A = 0x4000;
  TCNT1 = 0;
  ENABLE_STEPPER_DRIVER_INTERRUPT();

  #if ENABLED(ADVANCE)
    #if defined(TCCR0A) && defined(WGM01)
      CBI(TCCR0A, WGM01);
      CBI(TCCR0A, WGM00);
    #endif
    e_steps[0] = e_steps[1] = e_steps[2] = e_steps[3] = e_steps[4] = e_steps[5] = 0;
    SBI(TIMSK0, OCIE0A);
  #endif //ADVANCE

  enable_endstops(true); // Start with endstops active. After homing they can be disabled
  sei();

  set_stepper_direction(); // Init directions to out_bits = 0
}


/**
 * Block until all buffered steps are executed
 */
void st_synchronize() { while (blocks_queued()) idle(); }

void st_set_position(const long& x, const long& y, const long& z, const long& e) {
  CRITICAL_SECTION_START;
  count_position[X_AXIS] = x;
  count_position[Y_AXIS] = y;
  count_position[Z_AXIS] = z;
  count_position[E_AXIS] = e;
  CRITICAL_SECTION_END;
}

void st_set_e_position(const long& e) {
  CRITICAL_SECTION_START;
  count_position[E_AXIS] = e;
  CRITICAL_SECTION_END;
}

long st_get_position(uint8_t axis) {
  CRITICAL_SECTION_START;
  long count_pos = count_position[axis];
  CRITICAL_SECTION_END;
  return count_pos;
}

float st_get_axis_position_mm(AxisEnum axis) {
  float axis_pos;
  #if MECH(COREXY) || MECH(COREYX) || MECH(COREXZ) || MECH(COREZX)
    if (axis == X_AXIS || axis == CORE_AXIS_2) {
      CRITICAL_SECTION_START;
      long  pos1 = count_position[A_AXIS],
            pos2 = count_position[CORE_AXIS_2];
      CRITICAL_SECTION_END;
      // ((a1+a2)+(a1-a2))/2 -> (a1+a2+a1-a2)/2 -> (a1+a1)/2 -> a1
      // ((a1+a2)-(a1-a2))/2 -> (a1+a2-a1+a2)/2 -> (a2+a2)/2 -> a2
      axis_pos = (pos1 + ((axis == X_AXIS) ? pos2 : -pos2)) / 2.0f;
    }
    else
      axis_pos = st_get_position(axis);
  #else
    axis_pos = st_get_position(axis);
  #endif

  return axis_pos / axis_steps_per_unit[axis];
}

void enable_all_steppers() {
  enable_x();
  enable_y();
  enable_z();
  enable_e0();
  enable_e1();
  enable_e2();
  enable_e3();
}

void disable_all_steppers() {
  disable_x();
  disable_y();
  disable_z();
  disable_e0();
  disable_e1();
  disable_e2();
  disable_e3();
}

void finishAndDisableSteppers() {
  st_synchronize();
  disable_all_steppers();
}

void quickStop() {
  cleaning_buffer_counter = 5000;
  DISABLE_STEPPER_DRIVER_INTERRUPT();
  while (blocks_queued()) plan_discard_current_block();
  current_block = NULL;
  ENABLE_STEPPER_DRIVER_INTERRUPT();
}

#if ENABLED(NPR2)
  void colorstep(long csteps,const bool direction) {
    enable_e1();
    //setup new step
    WRITE(E1_DIR_PIN,(INVERT_E1_DIR)^direction);
    //perform step
    for(long i=0; i<=csteps; i++){
      WRITE(E1_STEP_PIN, !INVERT_E_STEP_PIN);
      delayMicroseconds(COLOR_SLOWRATE);
      WRITE(E1_STEP_PIN, INVERT_E_STEP_PIN);
      delayMicroseconds(COLOR_SLOWRATE);
    }
  }  
#endif //NPR2

#if ENABLED(BABYSTEPPING)

  // MUST ONLY BE CALLED BY AN ISR,
  // No other ISR should ever interrupt this!
  void babystep(const uint8_t axis, const bool direction) {

    #define _ENABLE(axis) enable_## axis()
    #define _READ_DIR(AXIS) AXIS ##_DIR_READ
    #define _INVERT_DIR(AXIS) INVERT_## AXIS ##_DIR
    #define _APPLY_DIR(AXIS, INVERT) AXIS ##_APPLY_DIR(INVERT, true)

    #define BABYSTEP_AXIS(axis, AXIS, INVERT) { \
        _ENABLE(axis); \
        uint8_t old_pin = _READ_DIR(AXIS); \
        _APPLY_DIR(AXIS, _INVERT_DIR(AXIS)^direction^INVERT); \
        _APPLY_STEP(AXIS)(!_INVERT_STEP_PIN(AXIS), true); \
        HAL::delayMicroseconds(2); \
        _APPLY_STEP(AXIS)(_INVERT_STEP_PIN(AXIS), true); \
        _APPLY_DIR(AXIS, old_pin); \
      }

    switch (axis) {

      case X_AXIS:
        BABYSTEP_AXIS(x, X, false);
        break;

      case Y_AXIS:
        BABYSTEP_AXIS(y, Y, false);
        break;

      case Z_AXIS: {

        #if !MECH(DELTA)

          BABYSTEP_AXIS(z, Z, BABYSTEP_INVERT_Z);

        #else // DELTA

          bool z_direction = direction ^ BABYSTEP_INVERT_Z;

          enable_x();
          enable_y();
          enable_z();
          uint8_t old_x_dir_pin = X_DIR_READ,
                  old_y_dir_pin = Y_DIR_READ,
                  old_z_dir_pin = Z_DIR_READ;
          //setup new step
          X_DIR_WRITE(INVERT_X_DIR ^ z_direction);
          Y_DIR_WRITE(INVERT_Y_DIR ^ z_direction);
          Z_DIR_WRITE(INVERT_Z_DIR ^ z_direction);
          // perform step
          X_STEP_WRITE(!INVERT_X_STEP_PIN);
          Y_STEP_WRITE(!INVERT_Y_STEP_PIN);
          Z_STEP_WRITE(!INVERT_Z_STEP_PIN);
          HAL::delayMicroseconds(1U);
          X_STEP_WRITE(INVERT_X_STEP_PIN);
          Y_STEP_WRITE(INVERT_Y_STEP_PIN);
          Z_STEP_WRITE(INVERT_Z_STEP_PIN);
          //get old pin state back.
          X_DIR_WRITE(old_x_dir_pin);
          Y_DIR_WRITE(old_y_dir_pin);
          Z_DIR_WRITE(old_z_dir_pin);

        #endif

      } break;

      default: break;
    }
  }

#endif //BABYSTEPPING

// From Arduino DigitalPotControl example
void digitalPotWrite(int address, int value) {
  #if HAS(DIGIPOTSS)
    digitalWrite(DIGIPOTSS_PIN, LOW); // take the SS pin low to select the chip
    SPI.transfer(address); //  send in the address and value via SPI:
    SPI.transfer(value);
    digitalWrite(DIGIPOTSS_PIN, HIGH); // take the SS pin high to de-select the chip:
    //HAL::delayMilliseconds(10);
  #else
    UNUSED(address);
    UNUSED(value);
  #endif
}

// Initialize Digipot Motor Current
void digipot_init() {
  #if HAS(DIGIPOTSS)
    const uint8_t digipot_motor_current[] = DIGIPOT_MOTOR_CURRENT;

    SPI.begin();
    pinMode(DIGIPOTSS_PIN, OUTPUT);
    for (int i = 0; i <= 4; i++) {
      //digitalPotWrite(digipot_ch[i], digipot_motor_current[i]);
      digipot_current(i, digipot_motor_current[i]);
    }
  #endif
  #if HAS(MOTOR_CURRENT_PWM_XY)
    pinMode(MOTOR_CURRENT_PWM_XY_PIN, OUTPUT);
    pinMode(MOTOR_CURRENT_PWM_Z_PIN, OUTPUT);
    pinMode(MOTOR_CURRENT_PWM_E_PIN, OUTPUT);
    digipot_current(0, motor_current_setting[0]);
    digipot_current(1, motor_current_setting[1]);
    digipot_current(2, motor_current_setting[2]);
    //Set timer5 to 31khz so the PWM of the motor power is as constant as possible. (removes a buzzing noise)
    TCCR5B = (TCCR5B & ~(_BV(CS50) | _BV(CS51) | _BV(CS52))) | _BV(CS50);
  #endif

  #if MB(ALLIGATOR)
    unsigned int digipot_motor = 0;
    for (uint8_t i = 0; i < 3 + DRIVER_EXTRUDERS; i++) {
      digipot_motor = 255 * (motor_current[i] / 2.5);
      ExternalDac::setValue(i, digipot_motor);
    }
  #endif//MB(ALLIGATOR)
}

void digipot_current(uint8_t driver, int current) {
  #if HAS(DIGIPOTSS)
    const uint8_t digipot_ch[] = DIGIPOT_CHANNELS;
    digitalPotWrite(digipot_ch[driver], current);
  #elif HAS(MOTOR_CURRENT_PWM_XY)
    switch (driver) {
      case 0: analogWrite(MOTOR_CURRENT_PWM_XY_PIN, 255L * current / MOTOR_CURRENT_PWM_RANGE); break;
      case 1: analogWrite(MOTOR_CURRENT_PWM_Z_PIN, 255L * current / MOTOR_CURRENT_PWM_RANGE); break;
      case 2: analogWrite(MOTOR_CURRENT_PWM_E_PIN, 255L * current / MOTOR_CURRENT_PWM_RANGE); break;
    }
  #else
    UNUSED(driver);
    UNUSED(current);
  #endif
}

void microstep_init() {
  #if HAS(MICROSTEPS_E1)
    pinMode(E1_MS1_PIN, OUTPUT);
    pinMode(E1_MS2_PIN, OUTPUT);
  #endif

  #if HAS(MICROSTEPS)
    pinMode(X_MS1_PIN, OUTPUT);
    pinMode(X_MS2_PIN, OUTPUT);
    pinMode(Y_MS1_PIN, OUTPUT);
    pinMode(Y_MS2_PIN, OUTPUT);
    pinMode(Z_MS1_PIN, OUTPUT);
    pinMode(Z_MS2_PIN, OUTPUT);
    pinMode(E0_MS1_PIN, OUTPUT);
    pinMode(E0_MS2_PIN, OUTPUT);
    const uint8_t microstep_modes[] = MICROSTEP_MODES;
    for (uint16_t i = 0; i < COUNT(microstep_modes); i++)
      microstep_mode(i, microstep_modes[i]);
  #endif
}

void microstep_ms(uint8_t driver, int8_t ms1, int8_t ms2) {
  if (ms1 >= 0) switch (driver) {
    case 0: digitalWrite(X_MS1_PIN, ms1); break;
    case 1: digitalWrite(Y_MS1_PIN, ms1); break;
    case 2: digitalWrite(Z_MS1_PIN, ms1); break;
    case 3: digitalWrite(E0_MS1_PIN, ms1); break;
    #if HAS(MICROSTEPS_E1)
      case 4: digitalWrite(E1_MS1_PIN, ms1); break;
    #endif
  }
  if (ms2 >= 0) switch (driver) {
    case 0: digitalWrite(X_MS2_PIN, ms2); break;
    case 1: digitalWrite(Y_MS2_PIN, ms2); break;
    case 2: digitalWrite(Z_MS2_PIN, ms2); break;
    case 3: digitalWrite(E0_MS2_PIN, ms2); break;
    #if PIN_EXISTS(E1_MS2)
      case 4: digitalWrite(E1_MS2_PIN, ms2); break;
    #endif
  }
}

void microstep_mode(uint8_t driver, uint8_t stepping_mode) {
  switch (stepping_mode) {
    case 1: microstep_ms(driver,  MICROSTEP1); break;
    case 2: microstep_ms(driver,  MICROSTEP2); break;
    case 4: microstep_ms(driver,  MICROSTEP4); break;
    case 8: microstep_ms(driver,  MICROSTEP8); break;
    case 16: microstep_ms(driver, MICROSTEP16); break;
    #if MB(ALLIGATOR)
      case 32: microstep_ms(driver, MICROSTEP32); break;
    #endif
  }
}

void microstep_readings() {
  ECHO_SM(DB, SERIAL_MICROSTEP_MS1_MS2);
  ECHO_M(SERIAL_MICROSTEP_X);
  ECHO_V(digitalRead(X_MS1_PIN));
  ECHO_EV(digitalRead(X_MS2_PIN));
  ECHO_SM(DB, SERIAL_MICROSTEP_Y);
  ECHO_V(digitalRead(Y_MS1_PIN));
  ECHO_EV(digitalRead(Y_MS2_PIN));
  ECHO_SM(DB, SERIAL_MICROSTEP_Z);
  ECHO_V(digitalRead(Z_MS1_PIN));
  ECHO_EV(digitalRead(Z_MS2_PIN));
  ECHO_SM(DB, SERIAL_MICROSTEP_E0);
  ECHO_V(digitalRead(E0_MS1_PIN));
  ECHO_EV(digitalRead(E0_MS2_PIN));
  #if HAS(MICROSTEPS_E1)
    ECHO_SM(DB, SERIAL_MICROSTEP_E1);
    ECHO_V(digitalRead(E1_MS1_PIN));
    ECHO_EV(digitalRead(E1_MS2_PIN));
  #endif
}

#if ENABLED(Z_DUAL_ENDSTOPS)
  void In_Homing_Process(bool state) { performing_homing = state; }
  void Lock_z_motor(bool state) { locked_z_motor = state; }
  void Lock_z2_motor(bool state) { locked_z2_motor = state; }
#endif
