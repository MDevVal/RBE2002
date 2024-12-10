#include "robot.h"
#include <message.pb.h>
// #include <IRdecoder.h>

void Robot::InitializeRobot(void) {
  chassis.InititalizeChassis();

  /**
   * Initialize the IR decoder. Declared extern in IRdecoder.h; see
   * robot-remote.cpp for instantiation and setting the pin.
   */
  // decoder.init();

  /**
   * Initialize the IMU and set the rate and scale to reasonable values.
   */
  imu.init();

  imu.setFullScaleGyro(imu.GYRO_FS500);
  imu.setGyroDataOutputRate(imu.ODR208);

  imu.setFullScaleAcc(imu.ACC_FS4);
  imu.setAccDataOutputRate(imu.ODR208);

  // The line sensor elements default to INPUTs, but we'll initialize anyways,
  // for completeness
  lineSensor.Initialize();

  EnterLineFollowing(10);
}

void Robot::EnterIdleState(void) {
  // Serial.println("-> IDLE");
  chassis.Stop();
  keyString = "";
  robotState = ROBOT_IDLE;
}

/**
 * Here is a good example of handling information differently, depending on the
 * state. If the Romi is not moving, we can update the bias (but be careful when
 * you first start up!). When it's moving, then we update the heading.
 */
void Robot::HandleOrientationUpdate(void) {
  prevEulerAngles = eulerAngles;
  if (robotState == ROBOT_IDLE) {
    // TODO: You'll need to add code to LSM6 to update the bias
    imu.updateGyroBias();
    imu.updateAccBias();
  }

  else // update orientation
  {

    // ACCEL ANGLES
    float accX = (imu.a.x - imu.accBias.y) * imu.mgPerLSB / 1000;
    float accY = (imu.a.y - imu.accBias.x) * imu.mgPerLSB / 1000;
    float accZ = (imu.a.z - imu.accBias.z) * imu.mgPerLSB / 1000;

    float accPitch = atan2(-accX, accZ) * 180 / PI;
    float accRoll = atan2(accY, accZ) * 180 / PI;

    // GYRO ANGLES
    float sdt = 1. / (imu.gyroODR) * imu.mdpsPerLSB / 1000.;
    float gyroX = (imu.g.x - imu.gyroBias.x) * sdt + prevEulerAngles.x;
    float gyroY = (imu.g.y - imu.gyroBias.y) * sdt + prevEulerAngles.y;
    float gyroZ = (imu.g.z - imu.gyroBias.z) * sdt + prevEulerAngles.z;

    // COMPLEMENTARY FILTER
    float kappa = 0.01;
    eulerAngles.x = kappa * accPitch + (1 - kappa) * gyroX;
    eulerAngles.y = kappa * accRoll + (1 - kappa) * gyroY;
    eulerAngles.z = gyroZ; // accelerometer doesnt give yaw

    float epsilon = 0.0001;
    imu.gyroBias.x = imu.gyroBias.x - epsilon / sdt * (accPitch - gyroX);
    imu.gyroBias.y = imu.gyroBias.y - epsilon / sdt * (accRoll - gyroY);

    // Keep the heading between 0 and 360
    eulerAngles.z = fmod(eulerAngles.z, 360);

#ifdef __IMU_DEBUG__
    Serial.println("pitch: \t" + String(eulerAngles.x));
    Serial.println("roll: \t" + String(eulerAngles.y));
#endif
  }
}

void Robot::EnterRamping(float speed) {
  baseSpeed = speed;
  onRamp = false;
  robotState = ROBOT_RAMPING;
}

void Robot::RampingUpdate(void) {
  if (robotState == ROBOT_RAMPING) {
    // Serial.println("doing ramp things: \t" + String(eulerAngles.x));
    LineFollowingUpdate();
    if (onRamp) {
      if (abs(eulerAngles.x) < 2) {
        EnterIdleState();
        onRamp = false;
      }
    } else {
      if (abs(eulerAngles.x) > 5) {
        onRamp = true;
      }
    }

    digitalWrite(13, onRamp);
  }
}

void Robot::RobotLoop(void) {
  /**
   * The main loop for your robot. Process both synchronous events (motor
   * control), and asynchronous events (IR presses, distance readings, etc.).
   */

  /**
   * Handle any IR remote keypresses.
   */
  // int16_t keyCode = decoder.getKeyCode();
  // if(keyCode != -1) HandleKeyCode(keyCode);

  /**
   * Check the Chassis timer, which is used for executing motor control
   */
  if (chassis.CheckChassisTimer()) {
    // add synchronous, pre-motor-update actions here
    if (robotState == ROBOT_LINING)
      LineFollowingUpdate();
    if (robotState == ROBOT_RAMPING)
      RampingUpdate();

    chassis.UpdateMotors();

    // add synchronous, post-motor-update actions here
  }

  /**
   * Check for any intersections
   */
  if (robotState == ROBOT_LINING && lineSensor.CheckIntersection())
    HandleIntersection();

  if (robotState == ROBOT_CENTERING && CheckCenteringComplete()){
    EnterIdleState();
    
    message_RomiData data = message_RomiData_init_default;
    data.has_gridLocation = true;
    data.gridLocation.x = iGrid;
    data.gridLocation.y = jGrid;
    ESPInterface.sendProtobuf(data, message_RomiData_fields, message_RomiData_size);
  }

    /**
     * Check for an IMU update
     */
    if(imu.checkForNewData())
    {
        HandleOrientationUpdate();
    }

    /**
     * Check for any messages from the ESP32
     */
    size_t msg_size;
    if (!ESPInterface.readUART(msg_size)) return;

    message_AprilTag tag = message_AprilTag_init_default;
    if (msg_size == message_AprilTag_size) {
        if (!ESPInterface.readProtobuf(tag, message_AprilTag_fields)) return;
        Serial.println("Tag ID: " + String(tag.id));

        // HandleAprilTag(tag);
    }

    message_ServerCommand data = message_ServerCommand_init_default;
    if (msg_size == message_ServerCommand_size) {

        // Decode the message from the Romi
        if (!ESPInterface.readProtobuf(data, message_ServerCommand_fields)) return;

        if (data.has_targetGridCell) {
            iTarget = data.targetGridCell.x;
            jTarget = data.targetGridCell.y;
        }

        if (data.has_state) switch (data.state) {
            case message_ServerCommand_State_IDLE:
                EnterIdleState();
                break;
            case message_ServerCommand_State_DRIVING:
                EnterLineFollowing(data.baseSpeed);
                break;
            case message_ServerCommand_State_LINING:
                EnterLineFollowing(data.baseSpeed);
                break;
            case message_ServerCommand_State_TURNING:
                EnterTurn(data.baseSpeed);
                break;
            case message_ServerCommand_State_RAMPING:
                EnterRamping(data.baseSpeed);
                break;
            case message_ServerCommand_State_SEARCHING:
                break;
            case message_ServerCommand_State_GIMMIE_THAT_TAG:
                break;
            case message_ServerCommand_State_TARGETING:
                break;
            case message_ServerCommand_State_WEIGHING:
                break;
            case message_ServerCommand_State_LIFTING:
                break;
            default:
                break;
        }
    }
}
