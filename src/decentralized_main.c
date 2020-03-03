#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "app.h"

#include "commander.h"

#include "FreeRTOS.h"
#include "task.h"

//#include "debug.h"

#include "param.h"
#include "log.h"

// my stuff
#include "estimator_kalman.h"
#include "radiolink.h"
#include "led.h"
#include "vector3.h"
// #include "simple_avoid.h"
#include "console.h"

#define MAX(a, b) ((a > b) ? a : b)
#define MIN(a, b) ((a < b) ? a : b)

#define OTHER_DRONES_ARRAY_SIZE 10  // size of the array containing the positions of other drones. must be at least as high as the highest id among all drones plus one.
#define DUMMY_VALUE 99999  // the xyz coordinates and velocities of non-connected drones will be set to this value

//#define DEBUG_MODULE "PUSH

typedef enum
{
  uninitialized,
  enginesOff,
  simpleAvoid,
  flock,
  debug1,
  debug2,
  debug3,
} State;

typedef struct _PacketData
{
  int id;
  Vector3 pos;
  Vector3 vel;
} PacketData;  // size: ? bytes + 12 bytes + 12 bytes  // max: 60 bytes


static PacketData packetData;  // the data that is send around via the p2p broadcast method
static Vector3 targetPosition;  // this drones's target position
static Vector3 lastPosition;  // this drones position in the last execution cycle, used to estimate velocity
static Vector3 otherPositions[OTHER_DRONES_ARRAY_SIZE];  // array of the positions of the other drones
static Vector3 otherVelocities[OTHER_DRONES_ARRAY_SIZE];  // array of the velocities of the other drones
static uint8_t droneAmount;  // amount of drones. SET DURING INITIALIZATION, DON'T CHANGE AT RUNTIME.
static uint8_t timer;
// variables for the avoidance algorithm, values are set from the pc. default values exist just in case something goes wrong.
static float forceFalloff = 1.5;
static float targetForce = 0.3;
static float avoidRange = 1.0;
static float avoidForce = 1.5;
static float maxLength = 1.0;
static float accBudget = 1.0;
static float zMiddle = 1.0;
static float xMax = 1.5;
static float yMax = 1.0;
static float zMax = 0.7;
static float wWallAvoid = 1.0;
static float wSeparation = 1.0;
static float wAlignment = 1.0;
static float wCohesion = 1.0;
static float wTargetSeek = 1.0;

#pragma region P2Pcomm
static void communicate()
{
  if (timer == 2 * packetData.id)
  {
    // consolePrintf("%d comm \n", packetData.id);
    P2PPacket packet;
    packet.port = 0;
    packet.size = sizeof(PacketData);
    memcpy(packet.data, &packetData, sizeof(PacketData));
    radiolinkSendP2PPacketBroadcast(&packet);
  }
}

void p2pCallbackHandler(P2PPacket *p)
{
  PacketData receivedPacketData;
  memcpy(&receivedPacketData, p->data, sizeof(PacketData));
  otherPositions[receivedPacketData.id] = receivedPacketData.pos;
  otherVelocities[receivedPacketData.id] = receivedPacketData.vel;
  // consolePrintf("%d <- id=%d\n", packetData.id, receivedPacketData.id);
}
#pragma endregion P2Pcomm

#pragma region Flocking
static Vector3 getFlockVector(bool *isInAvoidRange)
{ 
  Vector3 flockVector = (Vector3){0, 0, 0};
  float remainingAcc = accBudget;

  // WALL AVOIDANCE
  Vector3 wallAvoidVector = (Vector3){0, 0, 0};
  float outsidedness = 0;
  if (packetData.pos.x > xMax) outsidedness += abs(packetData.pos.x - xMax);
  if (packetData.pos.x < -xMax) outsidedness += abs(packetData.pos.x + xMax);
  if (packetData.pos.y > yMax) outsidedness += abs(packetData.pos.y - yMax);
  if (packetData.pos.y < -yMax) outsidedness += abs(packetData.pos.y + yMax);
  if (packetData.pos.z > (zMiddle + zMax)) outsidedness += abs(packetData.pos.z - (zMiddle + zMax));
  if (packetData.pos.z < (zMiddle - zMax)) outsidedness += abs(packetData.pos.z - (zMiddle - zMax));
  
  if (outsidedness > 0)
  {
    wallAvoidVector = sub(packetData.pos, (Vector3){0, 0, zMiddle});
    wallAvoidVector = norm(wallAvoidVector);
    wallAvoidVector = mul(wallAvoidVector, outsidedness);
  }
  wallAvoidVector = mul(wallAvoidVector, wWallAvoid);
  addToFlockVector(&flockVector, wallAvoidVector, &remainingAcc);

  // SEPARATION
  Vector3 separationVector = (Vector3){0, 0, 0};
  *isInAvoidRange = false;
  for (int i = 0; i < OTHER_DRONES_ARRAY_SIZE; i++)
  {
    Vector3 otherToDrone = sub(otherPositions[i], packetData.pos);
    float distance = magnitude(otherToDrone);
    if(distance < avoidRange)
    {
      *isInAvoidRange = true;
      otherToDrone = norm(otherToDrone);
      otherToDrone = mul(otherToDrone, 1 - (distance / avoidRange));
      // otherToDrone = mul(otherToDrone, avoidForce);
      separationVector = add(separationVector, otherToDrone);
    }
  }
  separationVector = mul(separationVector, wSeparation);
  addToFlockVector(&flockVector, separationVector, &remainingAcc);

  // ALIGNMENT
  Vector3 alignmentVector = packetData.vel;
  int dronesInAlignRange = 1;
  for (int i = 0; i < OTHER_DRONES_ARRAY_SIZE; i++)
  {
    Vector3 otherToDrone = sub(otherPositions[i], packetData.pos);
    float distance = magnitude(otherToDrone);
    if(distance < avoidRange)
    {
      dronesInAlignRange += 1;
      alignmentVector = add(alignmentVector, otherVelocities[i]);
    }
  }
  alignmentVector = mul(alignmentVector, 1 / dronesInAlignRange);
  alignmentVector = mul(alignmentVector, wAlignment);
  addToFlockVector(&flockVector, alignmentVector, &remainingAcc);

  // COHESION
  Vector3 cohesionVector = (Vector3){0, 0, 0};
  int dronesInCohesionRange = 0;
  for (int i = 0; i < OTHER_DRONES_ARRAY_SIZE; i++)
  {
    Vector3 otherToDrone = sub(otherPositions[i], packetData.pos);
    float distance = magnitude(otherToDrone);
    if(distance < avoidRange)
    {
      dronesInCohesionRange += 1;
      cohesionVector = add(cohesionVector, otherPositions[i]);
    }
  }
  if (dronesInCohesionRange > 0)
  {
    cohesionVector = mul(cohesionVector, 1 / dronesInCohesionRange);
  }
  cohesionVector = mul(cohesionVector, wCohesion);
  addToFlockVector(&flockVector, cohesionVector, &remainingAcc);

  // TARGET SEEKING
  Vector3 targetVector = sub(packetData.pos, targetPosition);
  if(magnitude(targetVector) > forceFalloff)
  {
    targetVector = norm(targetVector);
  }
  else
  {
    targetVector = mul(targetVector, 1 / forceFalloff);
  }
  targetVector = mul(targetVector, wTargetSeek);
  addToFlockVector(&flockVector, targetVector, &remainingAcc);

  return flockVector;
}

void addToFlockVector(Vector3 *flockVector, Vector3 vector, float *remainingAcc)
{
  if (*remainingAcc < 0)
  {
    return;
  }
  float length = magnitude(vector);
  if (*remainingAcc > length)
  {
    *flockVector = add(*flockVector, vector);
  }
  else
  {
    *flockVector = add(*flockVector, clamp(vector, *remainingAcc));
  }
  *remainingAcc -= length;
}
#pragma endregion Flocking

#pragma region SimpleAvoid
static Vector3 getSimpleAvoidVector(bool *isInAvoidRange)
{
  Vector3 vector = getTargetVector();
  vector = add(vector, getAvoidVector(isInAvoidRange));  // check if this works
  return clamp(vector, maxLength);
}

static Vector3 getTargetVector()
{
  Vector3 droneToTarget = sub(packetData.pos, targetPosition);
  if(magnitude(droneToTarget) > forceFalloff)
  {
    droneToTarget = norm(droneToTarget);
  }
  else
  {
    droneToTarget = mul(droneToTarget, 1 / forceFalloff);
  }
  droneToTarget = mul(droneToTarget, targetForce);
  return droneToTarget;
}

static Vector3 getAvoidVector(bool *isInAvoidRange)
{
  *isInAvoidRange = false;
  // other drones stuff (maybe quit early if otherPositions[i].x == DUMMY_VALUE?)
  Vector3 sum = (Vector3){0, 0, 0};
  for (int i = 0; i < OTHER_DRONES_ARRAY_SIZE; i++)
  {
    Vector3 otherToDrone = sub(otherPositions[i], packetData.pos);
    float distance = magnitude(otherToDrone);
    if(distance < avoidRange)
    {
      *isInAvoidRange = true;
      otherToDrone = norm(otherToDrone);
      otherToDrone = mul(otherToDrone, 1 - (distance / avoidRange));
      otherToDrone = mul(otherToDrone, avoidForce);
      sum = add(sum, otherToDrone);
    }
  }
  return sum;
}
#pragma endregion SimpleAvoid

#pragma region Setpoints_LED
static void setHoverSetpoint(setpoint_t *sp, float x, float y, float z)
{
  sp->mode.x = modeAbs;
  sp->mode.y = modeAbs;
  sp->mode.z = modeAbs;
  sp->mode.yaw = modeAbs;

  sp->position.x = x;
  sp->position.y = y;
  sp->position.z = z;
  sp->attitude.yaw = 0;
}

static void shutOffEngines(setpoint_t *sp)
{
  sp->mode.x = modeDisable;
  sp->mode.y = modeDisable;
  sp->mode.z = modeDisable;
  sp->mode.yaw = modeDisable;
}

static void ledIndicateDetection(bool isInRange)
{
  if (isInRange)
  {
    ledSetAll();
  }
  else
  {
    ledClearAll();
  }
}
#pragma endregion Setpoints_LED

// ENTRY POINT
void appMain()
{
  static point_t kalmanPosition;
  static point_t kalmanVelocity;
  static setpoint_t setpoint;
  static State state = uninitialized;
  bool isInAvoidRange = false;  // true if the drone is close within avoidRange of another one
  bool isLanding = false;  // true if the drone is requested to land
  
  timer = 0;

  #pragma region Param_Log
  // drone.cmd value meanings:
  // 1:   start
  // 2:   land
  // 3:   debug1
  // 4:   debug2
  // 5:   idle
  // 6:   reset timer
  // 100: used to trigger initialization
  // be careful not to use these values for something else
  static int8_t droneCmd = 0;
  // drone.mode meaning:
  // 0: simple potential field avoidance and target approach
  // 1: boid flocking
  static int8_t droneMode = 0;

  // parameters can be written from the pc and read by the drone
  PARAM_GROUP_START(drone)
  PARAM_ADD(PARAM_UINT8, amount, &droneAmount)
  PARAM_ADD(PARAM_UINT8, id, &packetData.id)
  PARAM_ADD(PARAM_INT8, cmd, &droneCmd)
  PARAM_ADD(PARAM_INT8, mode, &droneCmd)
  PARAM_ADD(PARAM_FLOAT, targetX, &targetPosition.x)
  PARAM_ADD(PARAM_FLOAT, targetY, &targetPosition.y)
  PARAM_ADD(PARAM_FLOAT, targetZ, &targetPosition.z)
  PARAM_ADD(PARAM_FLOAT, forceFalloff, &forceFalloff)
  PARAM_ADD(PARAM_FLOAT, targetForce, &targetForce)
  PARAM_ADD(PARAM_FLOAT, avoidRange, &avoidRange)
  PARAM_ADD(PARAM_FLOAT, avoidForce, &avoidForce)
  PARAM_ADD(PARAM_FLOAT, maxLength, &maxLength)
  PARAM_ADD(PARAM_FLOAT, accBudget, &accBudget)
  PARAM_ADD(PARAM_FLOAT, zMiddle, &zMiddle)
  PARAM_ADD(PARAM_FLOAT, xMax, &xMax)
  PARAM_ADD(PARAM_FLOAT, yMax, &yMax)
  PARAM_ADD(PARAM_FLOAT, zMax, &zMax)
  PARAM_ADD(PARAM_FLOAT, wWallAvoid, &wWallAvoid)
  PARAM_ADD(PARAM_FLOAT, wSeparation, &wSeparation)
  PARAM_ADD(PARAM_FLOAT, wAlignment, &wAlignment)
  PARAM_ADD(PARAM_FLOAT, wTargetSeek, &wTargetSeek)
  PARAM_GROUP_STOP(drone)

  // debug variables which can be written and read from the pc and the drone
  static float dbgflt = 0;
  static uint8_t dbgchr = 0;
  static int dbgint = 0;
  PARAM_GROUP_START(dbg)
  PARAM_ADD(PARAM_FLOAT, flt, &dbgflt)
  PARAM_ADD(PARAM_UINT8, chr, &dbgchr)
  PARAM_ADD(PARAM_INT32, int, &dbgint)
  PARAM_GROUP_STOP(dbg)
  LOG_GROUP_START(dbg)
  LOG_ADD(LOG_FLOAT, flt, &dbgflt)
  PARAM_ADD(LOG_UINT8, chr, &dbgchr)
  PARAM_ADD(LOG_INT32, int, &dbgint)
  LOG_GROUP_STOP(dbg)
  #pragma endregion Param_Log

  p2pRegisterCB(p2pCallbackHandler);

  // put PacketData structs with out of bounds values into the otherPositions array
  for (int i = 0; i < OTHER_DRONES_ARRAY_SIZE; i++)
  {
    otherPositions[i] = (Vector3){DUMMY_VALUE, DUMMY_VALUE, DUMMY_VALUE};
    otherVelocities[i] = (Vector3){DUMMY_VALUE, DUMMY_VALUE, DUMMY_VALUE};
  }

  while (1)
  {
    // vTaskDelay(M2T(10));
    vTaskDelay(M2T(10));
    timer += 1;
    if(timer == 2 * 10)
    {
      timer = 0;
    }

    // don't execute the entire while loop before initialization happend
    if (state == uninitialized)
    {
      if (droneCmd == 100)
      {
        state = enginesOff;
        droneCmd = 0;
      }
      continue;
    }

    estimatorKalmanGetEstimatedPos(&kalmanPosition);
    // put kalman position in this drone's packetData struct
    packetData.pos.x = kalmanPosition.x;
    packetData.pos.y = kalmanPosition.y;
    packetData.pos.z = kalmanPosition.z;
    // estimate velocity and put it in this drone's packetData struct
    packetData.vel.x = kalmanPosition.x - lastPosition.x;
    packetData.vel.y = kalmanPosition.y - lastPosition.y;
    packetData.vel.z = kalmanPosition.z - lastPosition.z;
    // update lastPosition for the next execution cycle
    lastPosition.x = kalmanPosition.x;
    lastPosition.y = kalmanPosition.y;
    lastPosition.z = kalmanPosition.z;

    switch (droneCmd)
    {
      case 0:
        break;
      case 1:  // start
        targetPosition.x = packetData.pos.x;
        targetPosition.y = packetData.pos.y;
        targetPosition.z = 0.7f;
        isLanding = false;
        if (droneMode == 0)
        {
          state = simpleAvoid;
        }
        if (droneMode == 1)
        {
          state = flock;
        }
        break;
      case 2:  // land
        targetPosition.x = packetData.pos.x;
        targetPosition.y = packetData.pos.y;
        targetPosition.z = -1.0f;
        isLanding = true;
        if (droneMode == 0)
        {
          state = simpleAvoid;
        }
        if (droneMode == 1)
        {
          state = flock;
        }
        break;
      case 3:  // debug1
        state = debug1;
        consolePrintf("Drone %d entered debug1 state \n", packetData.id);
        break;
      case 4:  // debug2
        state = debug2;
        consolePrintf("Drone %d entered debug2 state \n", packetData.id);
        break;
      case 5:  // off
        state = enginesOff;
        consolePrintf("Drone %d entered enginesOff state \n", packetData.id);
        break;
      case 6:  // reset
        timer = 0;
        consolePrintf("Timer reset for drone %d \n", packetData.id);
        break;
      case 10:  // info
        consolePrintf("%d: target x=%.2f y=%.2f z=%.2f \n", packetData.id, (double)targetPosition.x, (double)targetPosition.y, (double)targetPosition.z);
        consolePrintf("%d: forceFalloff=%.2f targetForce=%.2f avoidRange=%.2f avoidForce=%.2f maxLength=%.2f \n", packetData.id, (double)forceFalloff, (double)targetForce, (double)avoidRange, (double)avoidForce, (double)maxLength);
        break;
      default:
        break;
    }
    droneCmd = 0;

    switch (state)
    {
      case simpleAvoid:
        communicate();
        // Vector3 moveVector = getTargetVector();
        // moveVector = add(moveVector, getAvoidVector(&isInAvoidRange));
        // moveVector = clamp(moveVector, maxLength);
        // moveVector = add(moveVector, packetData.pos);
        Vector3 moveVector = add(packetData.pos, getSimpleAvoidVector(&isInAvoidRange));
        // consolePrintf("%d: x=%.2f y=%.2f z=%.2f \n", packetData.id, (double)moveVector.x, (double)moveVector.y, (double)moveVector.z);
        setHoverSetpoint(&setpoint, moveVector.x, moveVector.y, moveVector.z);

        ledIndicateDetection(isInAvoidRange);
        if (isLanding && packetData.pos.z < 0.15f)
        {
          state = enginesOff;
          consolePrintf("Drone %d entered enginesOff state \n", packetData.id);
        }
        break;
      case flock:
        communicate();
        Vector3 moveVector = add(packetData.pos, getFlockVector(&isInAvoidRange));
        setHoverSetpoint(&setpoint, moveVector.x, moveVector.y, moveVector.z);

        ledIndicateDetection(isInAvoidRange);
        if (isLanding && packetData.pos.z < 0.15f)
        {
          state = enginesOff;
          consolePrintf("Drone %d entered enginesOff state \n", packetData.id);
        }
        break;
      case enginesOff:
        communicate();
        getAvoidVector(&isInAvoidRange);
        shutOffEngines(&setpoint);
        ledIndicateDetection(isInAvoidRange);
        break;
      case debug1:
        communicate();
        Vector3 avoidVector = getAvoidVector(&isInAvoidRange);
        if (packetData.id == 4)
        {
          consolePrintf("%d: x=%.2f y=%.2f z=%.2f \n", packetData.id, (double)avoidVector.x, (double)avoidVector.y, (double)avoidVector.z);
        }
        ledIndicateDetection(isInAvoidRange);
        break;
      case debug2:
        consolePrintf("%d: x=%.2f y=%.2f z=%.2f \n", packetData.id, (double)packetData.pos.x, (double)packetData.pos.y, (double)packetData.pos.z);
        break;
      case uninitialized:  // this case should never occur since the drone should have been initialized before the switch statement
      default:
        ledClearAll();
        shutOffEngines(&setpoint);
        break;
    }
    commanderSetSetpoint(&setpoint, 3);
  }
}


// SOME OLD STUFF

// ledClearAll();
// ledSetAll();
// ledSet(LED_BLUE_L, true);

//int size = sizeof(array) / sizeof(array[0]);

// static void approachTargetAvoidOthers(setpoint_t *sp, bool *isInAvoidRange)
// {
//   Vector3 dronePosition = packetData.pos;
//   Vector3 sum = (Vector3){0, 0, 0};
//   *isInAvoidRange = false;
//   // target stuff
//   Vector3 droneToTarget = sub(dronePosition, targetPosition);
//   if(magnitude(droneToTarget) > forceFalloff)
//   {
//     droneToTarget = norm(droneToTarget);
//   }
//   else
//   {
//     droneToTarget = mul(droneToTarget, 1 / forceFalloff);
//   }
//   droneToTarget = mul(droneToTarget, targetForce);
//   sum = add(sum, droneToTarget);
//   // other drones stuff (maybe quit early if otherPositions[i].x == DUMMY_VALUE?)
//   for (int i = 0; i < OTHER_DRONES_ARRAY_SIZE; i++)
//   {
//     Vector3 otherToDrone = sub(otherPositions[i], dronePosition);
//     float distance = magnitude(otherToDrone);
//     if(distance < avoidRange)
//     {
//       *isInAvoidRange = true;
//       otherToDrone = norm(otherToDrone);
//       otherToDrone = mul(otherToDrone, 1 - (distance / avoidRange));
//       otherToDrone = mul(otherToDrone, avoidForce);
//       sum = add(sum, otherToDrone);
//     }
//   }
//   // add sum of forces to drone position and apply as setpoint
//   if (packetData.id == 4)
//   {
//     consolePrintf("%d: x=%.2f y=%.2f z=%.2f \n", packetData.id, (double)sum.x, (double)sum.y, (double)sum.z);
//   }
//   sum = add(dronePosition, sum);
//   setHoverSetpoint(sp, sum.x, sum.y, sum.z);
// }


      // case starting:
      //   moveVertical(&setpoint, 0.4);
      //   if (packetData.pos.z > 0.7f)
      //   {
      //     targetPosition.x = packetData.pos.x;
      //     targetPosition.y = packetData.pos.y;
      //     targetPosition.z = packetData.pos.z;
      //     consolePrintf("%d entered simpleAvoid state x=%.2f y=%.2f z=%.2f \n", packetData.id, (double)targetPosition.x, (double)targetPosition.y, (double)targetPosition.z);
      //     state = simpleAvoid;
      //   }
      //   break;
      // case landing:
      //   moveVertical(&setpoint, -0.4);
      //   if (packetData.pos.z < 0.05f)
      //   {
      //     state = enginesOff;
      //     consolePrintf("Drone %d entered enginesOff state \n", packetData.id);
      //   }
      //   break;

      
// static void moveVertical(setpoint_t *sp, float zVelocity)
// {
//   sp->mode.x = modeVelocity;
//   sp->mode.y = modeVelocity;
//   sp->mode.z = modeVelocity;
//   sp->mode.yaw = modeAbs;

//   sp->velocity.x = 0.0;
//   sp->velocity.y = 0.0;
//   sp->velocity.z = zVelocity;
//   sp->attitudeRate.yaw = 0.0;
// }